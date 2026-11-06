// The account calls Splatoon imports beyond TotK Recomp's ov_system.c: network service account (online identity)
// queries and the profile image. The user here is offline: those take the UserHandle that ov_system.c hands out,
// which the SDK's own code cannot read, so each one answers on the host that no online account exists.
#include <string.h>

#include "host.h"

// nn::account results (module 124). The exact descriptions do not matter to the game, only that they are failures.
#define ACCOUNT_R_NSA_UNAVAILABLE (124u | (0x1e0u << 9))
#define ACCOUNT_R_NO_PROFILE_IMAGE (124u | (0xd0u << 9))

// Result IsNetworkServiceAccountAvailable(bool *out, const UserHandle &)
static void ov_nsa_available(Cpu *c) {
    uint8_t *out = arg_p(c, 0);
    if (out) *out = 0;
    ret_x(c, 0);
}
// Result GetNetworkServiceAccountId(NetworkServiceAccountId *, const UserHandle &)
// Result EnsureNetworkServiceAccountAvailable(const UserHandle &)
// Result LoadNetworkServiceAccountIdTokenCache(size_t *, char *, size_t, const UserHandle &)
// Result EnsureNetworkServiceAccountIdTokenCacheAsync(AsyncContext *, const UserHandle &)
static void ov_nsa_unavailable(Cpu *c) {
    static int n;
    if (n++ < 4) {
        char lr[128];
        rt_log("account: network service account requested (from %s): offline user, unavailable",
               ldr_describe(c->x[30], lr, sizeof lr));
    }
    ret_x(c, ACCOUNT_R_NSA_UNAVAILABLE);
}
// Result LoadProfileImage(size_t *out_size, void *buffer, size_t size, const Uid &): no picture
static void ov_profile_image(Cpu *c) {
    uint64_t *size = arg_p(c, 0);
    if (size) *size = 0;
    ret_x(c, ACCOUNT_R_NO_PROFILE_IMAGE);
}

HOST_OVERRIDE("_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE", ov_nsa_available);
HOST_OVERRIDE("_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE",
              ov_nsa_unavailable);
HOST_OVERRIDE("_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE", ov_nsa_unavailable);
HOST_OVERRIDE("_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE", ov_nsa_unavailable);
HOST_OVERRIDE("_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE",
              ov_nsa_unavailable);
HOST_OVERRIDE("_ZN2nn7account16LoadProfileImageEPmPvmRKNS0_3UidE", ov_profile_image);
