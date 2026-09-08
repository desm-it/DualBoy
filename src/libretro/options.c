/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "libretro/options.h"

#include <libretro.h>

#include <stdbool.h>
#include <string.h>

#define DUALBOY_OPTION_MODE "dualboy_mode"
#define DUALBOY_OPTION_LAYOUT "dualboy_layout"
#define DUALBOY_OPTION_PLAYER1_CONTROLLER "dualboy_player1_controller"
#define DUALBOY_OPTION_PLAYER2_CONTROLLER "dualboy_player2_controller"
/* Keep the original public key so existing per-core overrides continue to
 * work; only its label and behavior are narrowed to presentation. */
#define DUALBOY_OPTION_SWAP_SCREENS "dualboy_swap_players"
#define DUALBOY_OPTION_AUDIO "dualboy_audio_source"

static struct retro_core_option_v2_category option_categories[] = {
    {
        "video",
        "Video",
        "Configure which screens are shown and how they are arranged.",
    },
    {
        "link",
        "Link and Players",
        "Configure local multiplayer transport and player-to-machine mapping.",
    },
    {
        "audio",
        "Audio",
        "Configure DualBoy audio output.",
    },
    {NULL, NULL, NULL},
};

static struct retro_core_option_v2_definition option_v2_definitions[] = {
    {
        DUALBOY_OPTION_MODE,
        "Display Mode",
        "Mode",
        "Show both machines or a single player's machine.",
        NULL,
        "video",
        {
            {"dual", "Dual"},
            {"player1", "Player 1 Only"},
            {"player2", "Player 2 Only"},
            {NULL, NULL},
        },
        "dual",
    },
    {
        DUALBOY_OPTION_LAYOUT,
        "Dual-screen Layout",
        "Layout",
        "Arrange both native-resolution screens side by side or vertically.",
        NULL,
        "video",
        {
            {"side_by_side", "Side by Side"},
            {"top_bottom", "Top/Bottom"},
            {NULL, NULL},
        },
        "side_by_side",
    },
    {
        DUALBOY_OPTION_NDS_RENDERER,
        "Nintendo DS Renderer",
        "NDS Renderer",
        "Select software or OpenGL rendering for Nintendo DS content. Changes apply live. Enabling OpenGL disables Local Link; enabling Local Link restores software rendering.",
        NULL,
        "video",
        {
            {"software", "Software"},
            {"opengl", "OpenGL"},
            {NULL, NULL},
        },
        "software",
    },
    {
        DUALBOY_OPTION_LINK,
        "Local Link",
        "Local Link",
        "Connect or disconnect the emulated cable or local wireless transport between both machines.",
        NULL,
        "link",
        {
            {"enabled", "Enabled"},
            {"disabled", "Disabled"},
            {NULL, NULL},
        },
        "enabled",
    },
    {
        DUALBOY_OPTION_PLAYER1_CONTROLLER,
        "Player 1 Controller",
        "Player 1 Controller",
        "Select which RetroArch controller port operates emulated Player 1. The same port may be assigned to both players.",
        NULL,
        "link",
        {
            {"port1", "Controller Port 1"},
            {"port2", "Controller Port 2"},
            {"port3", "Controller Port 3"},
            {"port4", "Controller Port 4"},
            {"port5", "Controller Port 5"},
            {NULL, NULL},
        },
        "port1",
    },
    {
        DUALBOY_OPTION_PLAYER2_CONTROLLER,
        "Player 2 Controller",
        "Player 2 Controller",
        "Select which RetroArch controller port operates emulated Player 2. The same port may be assigned to both players.",
        NULL,
        "link",
        {
            {"port1", "Controller Port 1"},
            {"port2", "Controller Port 2"},
            {"port3", "Controller Port 3"},
            {"port4", "Controller Port 4"},
            {"port5", "Controller Port 5"},
            {NULL, NULL},
        },
        "port2",
    },
    {
        DUALBOY_OPTION_SWAP_SCREENS,
        "Swap Screens",
        "Swap Screens",
        "Exchange the positions of the two machine screens without changing controller assignments.",
        NULL,
        "video",
        {
            {"disabled", "Disabled"},
            {"enabled", "Enabled"},
            {NULL, NULL},
        },
        "disabled",
    },
    {
        DUALBOY_OPTION_AUDIO,
        "Audio Source",
        "Source",
        "Output player 1 audio or disable audio output.",
        NULL,
        "audio",
        {
            {"player1", "Player 1"},
            {"disabled", "Disabled"},
            {NULL, NULL},
        },
        "player1",
    },
    {NULL, NULL, NULL, NULL, NULL, NULL, {{NULL, NULL}}, NULL},
};

static struct retro_core_options_v2 option_v2 = {
    option_categories,
    option_v2_definitions,
};

static struct retro_core_option_definition option_v1_definitions[] = {
    {
        DUALBOY_OPTION_MODE,
        "Display Mode",
        "Show both machines or a single player's machine.",
        {
            {"dual", "Dual"},
            {"player1", "Player 1 Only"},
            {"player2", "Player 2 Only"},
            {NULL, NULL},
        },
        "dual",
    },
    {
        DUALBOY_OPTION_LAYOUT,
        "Dual-screen Layout",
        "Arrange both native-resolution screens side by side or vertically.",
        {
            {"side_by_side", "Side by Side"},
            {"top_bottom", "Top/Bottom"},
            {NULL, NULL},
        },
        "side_by_side",
    },
    {
        DUALBOY_OPTION_NDS_RENDERER,
        "Nintendo DS Renderer",
        "Select software or OpenGL rendering for Nintendo DS content. Changes apply live. Enabling OpenGL disables Local Link; enabling Local Link restores software rendering.",
        {
            {"software", "Software"},
            {"opengl", "OpenGL"},
            {NULL, NULL},
        },
        "software",
    },
    {
        DUALBOY_OPTION_LINK,
        "Local Link",
        "Connect or disconnect the emulated cable or local wireless transport between both machines.",
        {
            {"enabled", "Enabled"},
            {"disabled", "Disabled"},
            {NULL, NULL},
        },
        "enabled",
    },
    {
        DUALBOY_OPTION_PLAYER1_CONTROLLER,
        "Player 1 Controller",
        "Select which RetroArch controller port operates emulated Player 1. The same port may be assigned to both players.",
        {
            {"port1", "Controller Port 1"},
            {"port2", "Controller Port 2"},
            {"port3", "Controller Port 3"},
            {"port4", "Controller Port 4"},
            {"port5", "Controller Port 5"},
            {NULL, NULL},
        },
        "port1",
    },
    {
        DUALBOY_OPTION_PLAYER2_CONTROLLER,
        "Player 2 Controller",
        "Select which RetroArch controller port operates emulated Player 2. The same port may be assigned to both players.",
        {
            {"port1", "Controller Port 1"},
            {"port2", "Controller Port 2"},
            {"port3", "Controller Port 3"},
            {"port4", "Controller Port 4"},
            {"port5", "Controller Port 5"},
            {NULL, NULL},
        },
        "port2",
    },
    {
        DUALBOY_OPTION_SWAP_SCREENS,
        "Swap Screens",
        "Exchange the positions of the two machine screens without changing controller assignments.",
        {
            {"disabled", "Disabled"},
            {"enabled", "Enabled"},
            {NULL, NULL},
        },
        "disabled",
    },
    {
        DUALBOY_OPTION_AUDIO,
        "Audio Source",
        "Output player 1 audio or disable audio output.",
        {
            {"player1", "Player 1"},
            {"disabled", "Disabled"},
            {NULL, NULL},
        },
        "player1",
    },
    {NULL, NULL, NULL, {{NULL, NULL}}, NULL},
};

static struct retro_variable option_legacy_definitions[] = {
    {DUALBOY_OPTION_MODE, "Display Mode; dual|player1|player2"},
    {DUALBOY_OPTION_LAYOUT,
     "Dual-screen Layout; side_by_side|top_bottom"},
    {DUALBOY_OPTION_NDS_RENDERER,
     "Nintendo DS Renderer; software|opengl"},
    {DUALBOY_OPTION_LINK, "Local Link; enabled|disabled"},
    {DUALBOY_OPTION_PLAYER1_CONTROLLER,
     "Player 1 Controller; port1|port2|port3|port4|port5"},
    {DUALBOY_OPTION_PLAYER2_CONTROLLER,
     "Player 2 Controller; port2|port1|port3|port4|port5"},
    {DUALBOY_OPTION_SWAP_SCREENS, "Swap Screens; disabled|enabled"},
    {DUALBOY_OPTION_AUDIO, "Audio Source; player1|disabled"},
    {NULL, NULL},
};

void dualboy_options_set_defaults(struct dualboy_options *options)
{
    if (options == NULL) {
        return;
    }

    options->mode = DUALBOY_MODE_DUAL;
    options->layout = DUALBOY_LAYOUT_SIDE_BY_SIDE;
    options->link_enabled = true;
    options->controller_ports[0] = 0U;
    options->controller_ports[1] = 1U;
    options->swap_screens = false;
    options->audio_player1 = true;
    options->nds_renderer = DUALBOY_VIDEO_RENDERER_SOFTWARE;
}

void dualboy_options_register(retro_environment_t environment)
{
    unsigned version = 0U;

    if (environment == NULL) {
        return;
    }

    if (!environment(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version)) {
        version = 0U;
    }

    if (version >= 2U) {
        (void)environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &option_v2);
    } else if (version >= 1U) {
        (void)environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS,
                          option_v1_definitions);
    } else {
        (void)environment(RETRO_ENVIRONMENT_SET_VARIABLES,
                          option_legacy_definitions);
    }
}

static const char *option_value(retro_environment_t environment,
                                const char *key)
{
    struct retro_variable variable = {key, NULL};

    if (!environment(RETRO_ENVIRONMENT_GET_VARIABLE, &variable)) {
        return NULL;
    }
    return variable.value;
}

static void read_mode(retro_environment_t environment,
                      struct dualboy_options *options)
{
    const char *value = option_value(environment, DUALBOY_OPTION_MODE);

    if (value == NULL) {
        return;
    }
    if (strcmp(value, "dual") == 0) {
        options->mode = DUALBOY_MODE_DUAL;
    } else if (strcmp(value, "player1") == 0) {
        options->mode = DUALBOY_MODE_PLAYER1;
    } else if (strcmp(value, "player2") == 0) {
        options->mode = DUALBOY_MODE_PLAYER2;
    }
}

static void read_layout(retro_environment_t environment,
                        struct dualboy_options *options)
{
    const char *value = option_value(environment, DUALBOY_OPTION_LAYOUT);

    if (value == NULL) {
        return;
    }
    if (strcmp(value, "side_by_side") == 0) {
        options->layout = DUALBOY_LAYOUT_SIDE_BY_SIDE;
    } else if (strcmp(value, "top_bottom") == 0) {
        options->layout = DUALBOY_LAYOUT_TOP_BOTTOM;
    }
}

static void read_link(retro_environment_t environment,
                      struct dualboy_options *options)
{
    const char *value = option_value(environment, DUALBOY_OPTION_LINK);

    if (value == NULL) {
        return;
    }
    if (strcmp(value, "enabled") == 0) {
        options->link_enabled = true;
    } else if (strcmp(value, "disabled") == 0) {
        options->link_enabled = false;
    }
}

static void read_nds_renderer(retro_environment_t environment,
                              struct dualboy_options *options)
{
    const char *value = option_value(environment, DUALBOY_OPTION_NDS_RENDERER);

    if (value == NULL) {
        return;
    }
    if (strcmp(value, "software") == 0) {
        options->nds_renderer = DUALBOY_VIDEO_RENDERER_SOFTWARE;
    } else if (strcmp(value, "opengl") == 0) {
        options->nds_renderer = DUALBOY_VIDEO_RENDERER_OPENGL;
    }
}

static void read_swap_screens(retro_environment_t environment,
                              struct dualboy_options *options)
{
    const char *value =
        option_value(environment, DUALBOY_OPTION_SWAP_SCREENS);

    if (value == NULL) {
        return;
    }
    if (strcmp(value, "enabled") == 0) {
        options->swap_screens = true;
    } else if (strcmp(value, "disabled") == 0) {
        options->swap_screens = false;
    }
}

static void read_controller_port(retro_environment_t environment,
                                 const char *key,
                                 unsigned *port)
{
    static const char *const values[DUALBOY_CONTROLLER_PORT_COUNT] = {
        "port1", "port2", "port3", "port4", "port5",
    };
    const char *value = option_value(environment, key);
    unsigned candidate;

    if (value == NULL || port == NULL) {
        return;
    }
    for (candidate = 0U; candidate < DUALBOY_CONTROLLER_PORT_COUNT;
         ++candidate) {
        if (strcmp(value, values[candidate]) == 0) {
            *port = candidate;
            return;
        }
    }
}

static void read_audio(retro_environment_t environment,
                       struct dualboy_options *options)
{
    const char *value = option_value(environment, DUALBOY_OPTION_AUDIO);

    if (value == NULL) {
        return;
    }
    if (strcmp(value, "player1") == 0) {
        options->audio_player1 = true;
    } else if (strcmp(value, "disabled") == 0) {
        options->audio_player1 = false;
    }
}

bool dualboy_options_read(retro_environment_t environment,
                          struct dualboy_options *options)
{
    struct dualboy_options previous;

    if (environment == NULL || options == NULL) {
        return false;
    }

    previous = *options;
    read_mode(environment, options);
    read_layout(environment, options);
    read_nds_renderer(environment, options);
    read_link(environment, options);
    read_controller_port(environment, DUALBOY_OPTION_PLAYER1_CONTROLLER,
                         &options->controller_ports[0]);
    read_controller_port(environment, DUALBOY_OPTION_PLAYER2_CONTROLLER,
                         &options->controller_ports[1]);
    read_swap_screens(environment, options);
    read_audio(environment, options);

    return previous.mode != options->mode ||
           previous.layout != options->layout ||
           previous.link_enabled != options->link_enabled ||
           previous.controller_ports[0] != options->controller_ports[0] ||
           previous.controller_ports[1] != options->controller_ports[1] ||
           previous.swap_screens != options->swap_screens ||
           previous.audio_player1 != options->audio_player1 ||
           previous.nds_renderer != options->nds_renderer;
}
