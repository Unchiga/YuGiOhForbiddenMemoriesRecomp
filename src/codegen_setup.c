/* codegen_setup.c — this title's configuration for the setup host.
 *
 * The download is a setup exe plus the recompiler SDK and the framework source;
 * it carries no Konami code and no Konami assets. On first run the player
 * points it at their own disc, it generates the C from that disc, builds into
 * build-release/, and every later run forwards straight there.
 *
 * Everything below is identity and paths. The flow itself is the framework's:
 * the runtime asks for the disc with the picker and acceptance gate it already
 * owns, then calls the two entry points here. Wired only when the build opts
 * into -DPSX_SETUP_HOST=ON, and compiled out entirely once generated/ exists —
 * a product build has nothing to set up.
 *
 * With recomp-ui linked (PSX_RECOMP_UI=ON) the runtime compiles that
 * launcher-less first run OUT and the launcher's first-run wizard drives the
 * same host instead. The wizard reaches this title through one extra entry
 * point, psx_game_codegen_setup_apply(), which hands it the same config.
 */

#if defined(PSX_HAS_SETUP_HOST) && !defined(PSX_HAS_GAME_DISPATCH)

#include <stddef.h>

#include "psxrecomp_codegen_host.h"

static const PsxrecompCodegenHostConfig kCodegenConfig = {
    /* .display_name          */ "Yu-Gi-Oh! Forbidden Memories",
    /* .project_root_env      */ "YGOFM_PROJECT_ROOT",
    /* .build_dir_env         */ "YGOFM_BUILD_DIR",
    /* .force_setup_env       */ "YGOFM_FORCE_SETUP",
    /* .psxrecomp_cli_relpath */ "psxrecomp/psxrecomp_cli.py",
    /* .seed_cfg_relpath      */ "game.toml",
    /* .game_toml_relpath     */ "game.toml",
    /* .gen_marker_relpath    */ "generated/SLUS_014.11_dispatch.c",
    /* .build_dir_name        */ "build-release",
    /* .cmake_target          */ "psx-runtime",
    /* .exe_basename          */ "Yu_Gi_Oh_Forbidden_Memories_Recompiled",
    /* .update_repo           */ "Unchiga/YuGiOhForbiddenMemoriesRecomp",
    /* .update_asset_format   */ "ygofm-%s-win-x64.zip",
    /* .prepare_note          */
    "Uses your own disc with the local psxrecomp SDK to generate BIOS + game "
    "C, then cmake --build. The game lives under build-release/; opening this "
    "setup exe again starts it from there.",
    /* .prepare_note_windows  */
    "Uses your own disc with the local psxrecomp SDK to generate BIOS + game "
    "C, then quits and builds in a helper console, because Windows will not "
    "relink an executable that is running. Afterwards this setup exe forwards "
    "to build-release/.",
    /* .prepare_note_no_cmake */
    "Uses your own disc with the local psxrecomp SDK to generate BIOS + game "
    "C. No build tools were found, so build into build-release/ yourself and "
    "then reopen this setup exe.",
    /* .openbios_only         */ 1,
    /* This title ships and runs the bundled OpenBIOS only. The wizard asks
     * for the disc and nothing else; a retail dump lying beside the install
     * is neither adopted nor offered, and CMakeLists links only the OpenBIOS
     * backend, so the built game cannot switch to one either. Retail BIOS
     * images were never part of what this build was verified against. */
};

int psx_game_codegen_setup_init(void) {
    return psxrecomp_codegen_host_init(&kCodegenConfig);
}

/* No progress callback: this build has no window yet and no launcher to draw
 * one in. The host's own steps already report to stderr, which is where a
 * console setup run is being watched. */
int psx_game_codegen_generate_and_build(const char *disc_path, char *out_exe,
                                        unsigned long out_cap, char *err_msg,
                                        unsigned long err_cap) {
    return psxrecomp_codegen_host_generate_and_build(
        disc_path, out_exe, (size_t)out_cap, err_msg, (size_t)err_cap,
        NULL, NULL);
}

int psx_game_codegen_update_check(char *local, unsigned long lcap, char *remote,
                                  unsigned long rcap, int force) {
    return psxrecomp_codegen_host_update_check(local, (size_t)lcap, remote,
                                               (size_t)rcap, force);
}

int psx_game_codegen_update_apply(char *helper, unsigned long hcap, char *err,
                                  unsigned long ecap) {
    return psxrecomp_codegen_host_update_apply(helper, (size_t)hcap, err,
                                               (size_t)ecap, NULL, NULL);
}

void psx_game_codegen_relaunch_or_exit(const char *disc_path) {
    psxrecomp_codegen_host_relaunch_or_exit(disc_path);
}

void psx_game_codegen_forward_if_built(int argc, char **argv) {
    psxrecomp_codegen_host_forward_if_built(&kCodegenConfig, argc, argv);
}

#if defined(PSX_HAS_RECOMP_LAUNCHER)
/* The recomp-ui wizard path. main.cpp calls this while filling the launcher's
 * game info (PSX_HAS_SETUP_WIZARD && PSX_HAS_GAME_CODEGEN); the host installs
 * its generate / rebuild / update callbacks from the config above, and the
 * wizard's "Generate & rebuild" then runs the same flow the console setup did.
 * Guarded on the launcher HEADER being reachable, which is what the type in
 * the signature needs; the runtime only calls it when the launcher is linked. */
void psx_game_codegen_setup_apply(RecompLauncherCGameInfo *gi) {
    psxrecomp_codegen_host_apply(gi, &kCodegenConfig);
}
#endif /* PSX_HAS_RECOMP_LAUNCHER */

#endif /* PSX_HAS_SETUP_HOST && !PSX_HAS_GAME_DISPATCH */
