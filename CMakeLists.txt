cmake_minimum_required(VERSION 3.20)
project(YuGiOhForbiddenMemoriesRecomp LANGUAGES C CXX)

set(PSXRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/psxrecomp" CACHE PATH
    "Path to psxrecomp")

# --- Opt-in product surfaces (OFF by default in the framework) -------------
# Scaffold prompts / flags fill these. Without recomp-ui, PSX_RECOMP_UI is OFF.
# Setup-host CI requires WIZARD_CMAKE_BLOCK / ENABLE_SETUP_WIZARD (FORCE_SETUP_HOST
# alone does not open first-run / Generate & rebuild).
set(PSX_RECOMP_UI OFF CACHE BOOL
    "No recomp-ui submodule in this scaffold" FORCE)
# Full netplay UI -- enable after adding recomp-ui + testing:
# if(EXISTS "${PSXRECOMP_ROOT}/lib/recomp-net/CMakeLists.txt")
#     set(PSX_NETPLAY ON CACHE BOOL "..." FORCE)
# endif()
# First-run setup wizard -- recomp-ui's modal. Not used: this project has no
# recomp-ui, and the wizard is the only thing in the setup host that needed it.
# set(PSX_SETUP_WIZARD ON CACHE BOOL "..." FORCE)
#
# The launcher-less first run instead. The setup exe asks for the player's disc
# with the runtime's own picker, generates the C from it, builds into
# build-release/ and forwards there on every later run -- so the download can
# carry the SDK and the framework source and no Konami code or assets at all.
# See src/codegen_setup.c, which compiles to nothing once generated/ exists.
set(PSX_SETUP_HOST ON CACHE BOOL
    "Launcher-less first-run generate + rebuild" FORCE)
include("${PSXRECOMP_ROOT}/runtime/runtime.cmake")

# --- What this title adds, and when ----------------------------------------
# Two different products come out of this one CMakeLists:
#
#   the SETUP HOST -- no generated/ yet. It links no game C, so it has nothing
#   to add features to and no reason to want the game's art. All it needs is
#   its own configuration, and it must NOT ask for a disc at configure time:
#   asking the player for one is the whole job it is built to do.
#
#   the GAME -- generated/ present. Everything below.
#
# Detected the same way the framework detects it, so the two cannot disagree.
set(YGOFM_GEN_MARKER
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/SLUS_014.11_dispatch.c")
set(YGOFM_HAS_GAME_C FALSE)
if(EXISTS "${YGOFM_GEN_MARKER}" AND NOT PSXRECOMP_FORCE_SETUP_HOST)
    set(YGOFM_HAS_GAME_C TRUE)
endif()

# Compiled either way. Guarded internally so it is empty in the game build.
set(YGOFM_GAME_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/codegen_setup.c")

if(YGOFM_HAS_GAME_C)
    # Everything specific to Yu-Gi-Oh! Forbidden Memories lives here, not in
    # the framework: the fusion assistant, the duel-rank meter, card drops and
    # the cheats. They register themselves at static-init time through
    # mod_plugins.h constructors, the overlay menu's row API, the debug-command
    # API and psx_game_hooks, so the runtime never names them and no other
    # title compiles them.
    file(GLOB YGOFM_FEATURE_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
    list(REMOVE_ITEM YGOFM_FEATURE_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/codegen_setup.c")
    list(APPEND YGOFM_GAME_SOURCES ${YGOFM_FEATURE_SOURCES})

    # --- Konami artwork is BUILD OUTPUT, not source ------------------------
    # Three sources are the game's own art -- the duel-rank badges and letters,
    # the CARD DROPS "New!" label, and the game's text font. They are derived
    # from the disc exactly the way generated/ is derived from the EXE, so like
    # generated/ they are gitignored and rebuilt from the PLAYER'S disc.
    # Nothing Konami-derived is committed or shipped.
    #
    # The disc is found the way the runtime finds it, so the ordinary dev loop
    # needs no new flag: an explicit -DYGOFM_DISC wins, then the disc this
    # build directory already verified, then the one the setup host recorded
    # beside the executable. With none of them, configure FAILS -- a build that
    # quietly produced a runtime with no art would look like a broken feature
    # rather than a missing input.
    if(NOT YGOFM_DISC)
        foreach(_ygofm_cfg
                "${CMAKE_BINARY_DIR}/disc_verified.cfg"
                "${CMAKE_CURRENT_SOURCE_DIR}/disc.cfg")
            if(NOT YGOFM_DISC AND EXISTS "${_ygofm_cfg}")
                file(STRINGS "${_ygofm_cfg}" _ygofm_lines)
                foreach(_ygofm_line IN LISTS _ygofm_lines)
                    if(NOT YGOFM_DISC AND _ygofm_line MATCHES "^path=")
                        string(REGEX REPLACE "^path=" "" YGOFM_DISC
                               "${_ygofm_line}")
                    elseif(NOT YGOFM_DISC AND EXISTS "${_ygofm_line}")
                        # Bare-path form: disc.cfg as the runtime writes it.
                        set(YGOFM_DISC "${_ygofm_line}")
                    endif()
                endforeach()
            endif()
        endforeach()
    endif()
    # Last resort: the working copy `psxrecomp_cli.py generate` prepares under
    # disc/. It runs before cmake in every documented build order, so this is
    # what makes the README's own sequence work with no extra flag -- without
    # it, following those instructions verbatim dies at the configure step.
    if(NOT YGOFM_DISC)
        file(GLOB _ygofm_prepared "${CMAKE_CURRENT_SOURCE_DIR}/disc/*.bin")
        if(_ygofm_prepared)
            list(GET _ygofm_prepared 0 YGOFM_DISC)
        endif()
    endif()
    if(NOT YGOFM_DISC OR NOT EXISTS "${YGOFM_DISC}")
        message(FATAL_ERROR
            "No disc image. The rank sprites, the New! label and the game font "
            "are baked from your own copy of the disc and are not "
            "distributed.\n"
            "Pass -DYGOFM_DISC=<path to the .bin>, or run the setup host, "
            "which asks for it once and remembers it.")
    endif()
    message(STATUS "ygofm: baking sprites/font from ${YGOFM_DISC}")

    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    set(YGOFM_ART_DIR "${CMAKE_BINARY_DIR}/game-assets")
    set(YGOFM_ART_SOURCES
        "${YGOFM_ART_DIR}/psx_rank_sprites.c"
        "${YGOFM_ART_DIR}/psx_cd_sprites.c"
        "${YGOFM_ART_DIR}/psx_fusion_font.c"
        "${YGOFM_ART_DIR}/psx_shop_skin.c")
    add_custom_command(
        OUTPUT ${YGOFM_ART_SOURCES}
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/disc_assets.py"
                "${YGOFM_DISC}" "${YGOFM_ART_DIR}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/disc_assets.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/sprite_extract.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/font_extract.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/sprite_spec.json"
        COMMENT "Baking sprites and font from the disc"
        VERBATIM)
    list(APPEND YGOFM_GAME_SOURCES ${YGOFM_ART_SOURCES})

    # --- and so are the drop tables ---------------------------------------
    # MODS > DROP TABLE VIEWER lists every duelist's drops, but the game keeps
    # only the CURRENT opponent's table resident, so they cannot be read out of
    # RAM the way card names and ATK/DEF are. They come off the player's disc
    # here for the same reason the art does, and are equally not committed.
    set(YGOFM_DROP_DB "${YGOFM_ART_DIR}/psx_drop_db.c")
    add_custom_command(
        OUTPUT ${YGOFM_DROP_DB}
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_drop_db.py"
                "${YGOFM_DISC}" "${YGOFM_ART_DIR}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_drop_db.py"
        COMMENT "Baking drop tables from the disc"
        VERBATIM)
    list(APPEND YGOFM_GAME_SOURCES ${YGOFM_DROP_DB})

    # --- the FREE DUEL portraits ------------------------------------------
    # Konami art again, and the one piece that is NOT derived from the disc:
    # the portrait atlas is compressed there. They are captured from a running
    # game into assets/duelist_icons/ (tools/gen_duelist_icons.py explains how)
    # and baked from those PNGs. Missing icons are emitted as NULL, so a tree
    # without them still builds and the viewer just draws a plain plate.
    set(YGOFM_ICON_SRC "${CMAKE_CURRENT_SOURCE_DIR}/assets/duelist_icons")
    set(YGOFM_ICONS "${YGOFM_ART_DIR}/psx_duelist_icons.c")
    file(GLOB YGOFM_ICON_PNGS CONFIGURE_DEPENDS "${YGOFM_ICON_SRC}/*.png")
    add_custom_command(
        OUTPUT ${YGOFM_ICONS}
        COMMAND ${Python3_EXECUTABLE}
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_duelist_icons.py"
                "${YGOFM_ICON_SRC}" "${YGOFM_ART_DIR}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_duelist_icons.py"
                ${YGOFM_ICON_PNGS}
        COMMENT "Baking duelist portraits"
        VERBATIM)
    list(APPEND YGOFM_GAME_SOURCES ${YGOFM_ICONS})
else()
    message(STATUS
        "ygofm: setup host — no game C, so no game features and no art")
endif()


# The release version, read from the VERSION file rather than repeated here.
# Without this the runtime compiled itself as "dev" while VERSION said 0.2.2,
# so the binary did not know its own release — which the update check compares
# against, and which the lobby pin already depended on. One source of truth.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" YGOFM_VERSION)
string(STRIP "${YGOFM_VERSION}" YGOFM_VERSION)
message(STATUS "ygofm: version ${YGOFM_VERSION}")

psxrecomp_add_game_runtime(psx-runtime
    # ENABLE_NETPLAY_IF_PRESENT
    # NETPLAY_LOBBY_URL ws://netplay.retcomm.net:8765
    # ENABLE_SETUP_WIZARD
    GAME_VERSION "${YGOFM_VERSION}"
    # Checked on every launch for a newer release; the player is prompted and
    # sent to the page. Comment out to disable the check for the whole build.
    UPDATE_REPO "Unchiga/YuGiOhForbiddenMemoriesRecomp"
    WINDOW_TITLE "Yu-Gi-Oh Forbidden Memories Recompiled"
    DEFAULT_GAME_CONFIG_PATH "game.toml"
    MAX_PLAYERS 2
    # LAUNCHER_BOXART "${CMAKE_CURRENT_SOURCE_DIR}/launcher_assets/img/boxart.tga"
    APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/psxrecomp.ico"
    GEN_MARKER "generated/SLUS_014.11_dispatch.c"
    GEN_FULL_GLOB "generated/SLUS_014.11_full_*.c"
    CODEGEN_SETUP_SOURCES ${YGOFM_GAME_SOURCES}
)

# The game's own headers sit beside its sources.
target_include_directories(psx-runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

# Project-owned mod catalog (this game's own manifests). Staged AFTER the
# framework's builtin copy so copy_directory merges into the same tree
# instead of being wiped by the framework's rm -rf. An always-run ALL target
# (not POST_BUILD) so editing only a manifest.toml re-stages without needing
# psx-runtime to relink.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/mods/preloaded/packages")
    add_custom_target(psx-stage-mods ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/mods/preloaded"
            "$<TARGET_FILE_DIR:psx-runtime>/mods"
        COMMENT "Staging project-owned mod catalog"
        VERBATIM)
    add_dependencies(psx-stage-mods psx-runtime)
endif()

# Stage runtime configs next to the exe for direct build/ launches. Only copy
# files that exist — never fail when game_options.toml is absent.
#
# This is its own always-run target rather than a POST_BUILD on psx-runtime,
# because POST_BUILD only fires when the exe RELINKS. Editing only game.toml
# left the staged copy stale, so the runtime kept reading the previous config
# while the build reported success — a config change that silently does nothing
# reads exactly like a feature that does nothing. copy_if_different makes the
# unconditional run free when nothing changed.
add_custom_target(psx-stage-configs ALL)
add_dependencies(psx-runtime psx-stage-configs)
foreach(_PSX_RUNTIME_CONFIG game.toml game_options.toml)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_PSX_RUNTIME_CONFIG}")
        add_custom_command(TARGET psx-stage-configs POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/${_PSX_RUNTIME_CONFIG}"
                "$<TARGET_FILE_DIR:psx-runtime>/${_PSX_RUNTIME_CONFIG}"
            COMMENT "Staging ${_PSX_RUNTIME_CONFIG}"
            VERBATIM)
    endif()
endforeach()

