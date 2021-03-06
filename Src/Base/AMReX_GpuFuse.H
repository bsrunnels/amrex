#ifndef AMREX_GPU_FUSE_H_
#define AMREX_GPU_FUSE_H_
#include <AMReX_Config.H>

#include <AMReX_Arena.H>
#include <AMReX_Vector.H>
#include <AMReX_Box.H>
#include <AMReX_TypeTraits.H>
#include <AMReX_GpuDevice.H>
#include <AMReX_GpuLaunchGlobal.H>
#include <cstring>
#include <memory>

namespace amrex {
namespace Gpu {

#ifdef AMREX_USE_GPU

#if defined(AMREX_USE_GPU_RDC) && (defined(AMREX_USE_CUDA) || (defined(AMREX_USE_HIP) && defined(AMREX_HIP_INDIRECT_FUNCTION)))

typedef void (*Lambda1DLauncher)(char*,int);
typedef void (*Lambda3DLauncher)(char*,int,int,int);
typedef void (*Lambda4DLauncher)(char*,int,int,int,int);
typedef void (*LambdaDtor)(char*);

struct FuseHelper {
    union LambdaLauncher {
        Lambda1DLauncher L1D;
        Lambda3DLauncher L3D;
        Lambda4DLauncher L4D;
    };
    // Device function pointer to wrapper around Lambda's operator()
    LambdaLauncher m_fp;
    std::size_t m_offset; // This tells us where the lambda object is.
    Box m_bx;
    int m_N;
};

static_assert(std::is_trivially_copyable<FuseHelper>(),"FuseHelper is not trivially copyable");

template <typename Lambda>
AMREX_GPU_DEVICE
void
LaunchLambda1D (char* buf, int i)
{
    (*reinterpret_cast<Lambda*>(buf))(i);
}

template <typename Lambda>
AMREX_GPU_DEVICE
void
LaunchLambda3D (char* buf, int i, int j, int k)
{
    (*reinterpret_cast<Lambda*>(buf))(i,j,k);
}

template <typename Lambda>
AMREX_GPU_DEVICE
void
LaunchLambda4D (char* buf, int i, int j, int k, int n)
{
    (*reinterpret_cast<Lambda*>(buf))(i,j,k,n);
}

template <typename Lambda>
void
DestroyLambda (char* buf)
{
    reinterpret_cast<Lambda*>(buf)->~Lambda();
}

template <typename Lambda>
std::size_t
PutLambda (Lambda const& f, char* buf)
{
    new (buf) Lambda(f);
    return Arena::align(sizeof(Lambda));
}

template <typename Lambda>
AMREX_GPU_DEVICE
auto GetLambda1DLauncherFnPtr (Lambda const& f, Lambda1DLauncher& launcher)
    -> decltype(f(0))
{
    amrex::ignore_unused(f);
    launcher = &LaunchLambda1D<Lambda>;
}

template <typename Lambda>
AMREX_GPU_DEVICE
auto GetLambda1DLauncherFnPtr (Lambda const& f, Lambda1DLauncher& launcher)
    -> decltype(f(0,Gpu::Handler{}))
{
    amrex::ignore_unused(f);
    launcher = nullptr;
}

template <typename Lambda>
AMREX_GPU_DEVICE
auto GetLambda3DLauncherFnPtr (Lambda const& f, Lambda3DLauncher& launcher)
    -> decltype(f(0,0,0))
{
    amrex::ignore_unused(f);
    launcher = &LaunchLambda3D<Lambda>;
}

template <typename Lambda>
AMREX_GPU_DEVICE
auto GetLambda3DLauncherFnPtr (Lambda const& f, Lambda3DLauncher& launcher)
    -> decltype(f(0,0,0,Gpu::Handler{}))
{
    amrex::ignore_unused(f);
    launcher = nullptr;
}

template <typename Lambda>
AMREX_GPU_DEVICE
auto GetLambda4DLauncherFnPtr (Lambda const& f, Lambda4DLauncher& launcher)
    -> decltype(f(0,0,0,0))
{
    amrex::ignore_unused(f);
    launcher = &LaunchLambda4D<Lambda>;
}

template <typename Lambda>
AMREX_GPU_DEVICE
auto GetLambda4DLauncherFnPtr (Lambda const& f, Lambda4DLauncher& launcher)
    -> decltype(f(0,0,0,0,Gpu::Handler{}))
{
    amrex::ignore_unused(f);
    launcher = nullptr;
}

template <typename Lambda>
void
PutLambda1DLauncher (FuseHelper* helper, Lambda const& f)
{
    static Lambda1DLauncher fp = nullptr;
    if (fp == nullptr) {
        launch_global<<<1,1>>>([=] AMREX_GPU_DEVICE ()
        {
            GetLambda1DLauncherFnPtr(f, helper->m_fp.L1D);
        });
        Gpu::synchronize();
        fp = helper->m_fp.L1D;
    } else {
        helper->m_fp.L1D = fp;
    }
}

template <typename Lambda>
void
PutLambda3DLauncher (FuseHelper* helper, Lambda const& f)
{
    static Lambda3DLauncher fp = nullptr;
    if (fp == nullptr) {
        launch_global<<<1,1>>>([=] AMREX_GPU_DEVICE ()
        {
            GetLambda3DLauncherFnPtr(f, helper->m_fp.L3D);
        });
        Gpu::synchronize();
        fp = helper->m_fp.L3D;
    } else {
        helper->m_fp.L3D = fp;
    }
}

template <typename Lambda>
void
PutLambda4DLauncher (FuseHelper* helper, Lambda const& f)
{
    static Lambda4DLauncher fp = nullptr;
    if (fp == nullptr) {
        launch_global<<<1,1>>>([=] AMREX_GPU_DEVICE ()
        {
            GetLambda4DLauncherFnPtr(f, helper->m_fp.L4D);
        });
        Gpu::synchronize();
        fp = helper->m_fp.L4D;
    } else {
        helper->m_fp.L4D = fp;
    }
}

template <typename Lambda>
void
PutLambdaDtor (LambdaDtor& lambda_dtor)
{
    lambda_dtor = &DestroyLambda<Lambda>;
}

class Fuser
{
public:

    Fuser ();
    ~Fuser ();
    Fuser (Fuser const&) = delete;
    Fuser& operator= (Fuser const&) = delete;

    template <typename F>
    void
    Register (Box const& bx, F&& f)
    {
        AMREX_ASSERT(!OpenMP::in_parallel());
        if (bx.isEmpty()) return;
        using Lambda = typename std::decay<F>::type;
        Register_doit<Lambda>(bx, 0, f);
        PutLambda3DLauncher<Lambda>(m_helper_buf+(m_nlambdas-1), f);
    }

    template <typename F>
    void
    Register (Box const& bx, int ncomp, F&& f)
    {
        AMREX_ASSERT(!OpenMP::in_parallel());
        if (bx.isEmpty()) return;
        using Lambda = typename std::decay<F>::type;
        Register_doit<Lambda>(bx, ncomp, f);
        PutLambda4DLauncher<Lambda>(m_helper_buf+(m_nlambdas-1), f);
    }

    template <typename T, typename F, typename M=std::enable_if_t<std::is_integral<T>::value> >
    void
    Register (T N, F&&f)
    {
        AMREX_ASSERT(!OpenMP::in_parallel());
        AMREX_ASSERT(sizeof(T) <= sizeof(int) ||
                     N < static_cast<T>(std::numeric_limits<int>::max()));
        if (N <= 0) return;
        using Lambda = typename std::decay<F>::type;
        Register_doit<Lambda>(Box(), N, f);
        PutLambda1DLauncher<Lambda>(m_helper_buf+(m_nlambdas-1), f);
    }

    void Launch ();

    static Fuser& getInstance ();
    static void Initialize ();
    static void Finalize ();

private:

    static std::unique_ptr<Fuser> m_instance;

    char* m_lambda_buf = nullptr;
    std::size_t m_nbytes_used_lambda_buf = 0;
    std::size_t m_nbytes_lambda_buf = 2*1024*1024;

    FuseHelper* m_helper_buf = nullptr;
    std::size_t m_nhelpers_buf = 1024;

    Vector<LambdaDtor> m_dtor_buf;

    int m_nlambdas = 0;

    void resize_lambda_buf ();
    void resize_helper_buf ();

    template <typename Lambda>
    void
    Register_doit (Box const& bx, int ncomp, Lambda const& f)
    {
        if (m_nlambdas >= static_cast<int>(m_nhelpers_buf)) {
            resize_helper_buf();
        }

        int ikernel = m_nlambdas++;
        new (m_helper_buf+ikernel) FuseHelper;
        m_helper_buf[ikernel].m_offset = m_nbytes_used_lambda_buf;
        m_helper_buf[ikernel].m_bx = bx;
        m_helper_buf[ikernel].m_N = ncomp;

        static_assert(std::alignment_of<Lambda>::value <= Arena::align_size,
                      "GpuFuse: alignment error");
        if (m_nbytes_used_lambda_buf + Arena::align(sizeof(Lambda)) > m_nbytes_lambda_buf) {
            resize_lambda_buf();
        }
        char* p_lambda = m_lambda_buf + m_nbytes_used_lambda_buf;
        std::size_t sz_lambda = PutLambda<Lambda>(f, p_lambda);
        m_nbytes_used_lambda_buf += sz_lambda;
        AMREX_ASSERT(m_nbytes_used_lambda_buf <= m_nbytes_lambda_buf);

        m_dtor_buf.push_back(nullptr);
        PutLambdaDtor<Lambda>(m_dtor_buf.back());
    }
};

template <typename F>
void
Register (Box const& bx, F&& f)
{
    Fuser::getInstance().Register(bx, std::forward<F>(f));
}

template <typename F>
void
Register (Box const& bx, int ncomp, F&& f)
{
    Fuser::getInstance().Register(bx, ncomp, std::forward<F>(f));
}

template <typename T, typename F, typename M=std::enable_if_t<std::is_integral<T>::value> >
void
Register (T N, F&&f)
{
    Fuser::getInstance().Register(N, std::forward<F>(f));
}

inline void
LaunchFusedKernels ()
{
    Fuser::getInstance().Launch();
}

#else

class Fuser
{
public:
    static Fuser& getInstance ();
    static void Initialize ();
    static void Finalize ();
private:
    static std::unique_ptr<Fuser> m_instance;
};

inline void LaunchFusedKernels () {}

#endif

Long getFuseSizeThreshold ();
Long setFuseSizeThreshold (Long new_threshold);
int getFuseNumKernelsThreshold ();
int setFuseNumKernelsThreshold (int new_threshold);
bool inFuseRegion ();
bool setFuseRegion (bool flag);
bool inFuseReductionRegion ();
bool setFuseReductionRegion (bool flag);

struct FuseSafeGuard
{
    explicit FuseSafeGuard (bool flag) noexcept
        : m_old(setFuseRegion(flag)) {}
    ~FuseSafeGuard () { setFuseRegion(m_old); }
private:
    bool m_old;
};

struct FuseReductionSafeGuard
{
    explicit FuseReductionSafeGuard (bool flag) noexcept
        : m_old(setFuseReductionRegion(flag)) {}
    ~FuseReductionSafeGuard () { setFuseReductionRegion(m_old); }
private:
    bool m_old;
};

#else

struct FuseSafeGuard
{
    explicit FuseSafeGuard (bool) {}
};

#endif

}}

#endif
