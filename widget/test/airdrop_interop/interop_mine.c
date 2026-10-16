#define AIRDROP_IMPLEMENTATION
#include "../../airdrop.h"
#include <unistd.h>
static int on_ask(void* u, const airdrop_ask* a) { (void)u; printf("ASK from %s model=%s files=%d first=%s type=%s icon=%zu\n", a->sender_name, a->sender_model, a->nfiles, a->nfiles ? a->files[0].name : "-", a->nfiles ? a->files[0].type : "-", a->icon_n); fflush(stdout); return 1; }
static void on_recv(void* u, const char* p) { (void)u; printf("RECEIVED %s\n", p); fflush(stdout); }
int main(int argc, char** argv) {
    ad__net_init();
    const char* mode = argv[1];
    airdrop_callbacks cb; memset(&cb, 0, sizeof(cb)); cb.on_ask = on_ask; cb.on_received = on_recv;
    airdrop_config cfg; memset(&cfg, 0, sizeof(cfg)); cfg.name = "Luna Interop"; cfg.download_dir = argc > 3 ? argv[3] : "."; cfg.port = getenv("AD_PORT") ? atoi(getenv("AD_PORT")) : 18880;
    airdrop_t* a = airdrop_create(&cfg, &cb);
    if (airdrop_start(a)) { printf("start failed: %s\n", airdrop_last_error(a)); return 1; }
    if (!strcmp(mode, "recv")) { printf("READY %d\n", airdrop_port(a)); fflush(stdout); sleep(atoi(argv[2])); }
    else { char err[200]; char nm[96]; const char* f[] = { argv[3] };
           printf("discover=%d name=%s\n", airdrop_discover(a, "::1", atoi(argv[2]), nm, sizeof(nm)), nm);
           printf("send=%d %s\n", airdrop_send(a, "::1", atoi(argv[2]), f, 1, err, sizeof(err)), err); }
    airdrop_destroy(a);
    return 0;
}
