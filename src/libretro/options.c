/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "libretro/options.h"

#include <libretro.h>

#include <stdbool.h>
#include <string.h>

#define DUALBOY_OPTION_MODE "dualboy_mode"
#define DUALBOY_OPTION_LAYOUT "dualboy_layout"
#define DUALBOY_OPTION_LINK "dualboy_link"
#define DUALBOY_OPTION_SWAP "dualboy_swap_players"
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
        "Configure the link cable and player-to-machine mapping.",
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
        DUALBOY_OPTION_LINK,
        "Link Cable",
        "Link Cable",
        "Connect or disconnect the emulated link cable between both machines.",
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
        DUALBOY_OPTION_SWAP,
        "Swap Players/Screens",
        "Swap Players/Screens",
        "Swap both controller assignments and screen positions together.",
        NULL,
        "link",
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
        DUALBOY_OPTION_LINK,
        "Link Cable",
        "Connect or disconnect the emulated link cable between both machines.",
        {
            {"enabled", "Enabled"},
            {"disabled", "Disabled"},
            {NULL, NULL},
        },
        "enabled",
    },
    {
        DUALBOY_OPTION_SWAP,
        "Swap Players/Screens",
        "Swap both controller assignments and screen positions together.",
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
    {DUALBOY_OPTION_LINK, "Link Cable; enabled|disabled"},
    {DUALBOY_OPTION_SWAP, "Swap Players/Screens; disabled|enabled"},
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
    options->swap_players = false;
    options->audio_player1 = true;
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

static void read_swap(retro_environment_t environment,
                      struct dualboy_options *options)
{
    const char *value = option_value(environment, DUALBOY_OPTION_SWAP);

    if (value == NULL) {
        return;
    }
    if (strcmp(value, "enabled") == 0) {
        options->swap_players = true;
    } else if (strcmp(value, "disabled") == 0) {
        options->swap_players = false;
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
    read_link(environment, options);
    read_swap(environment, options);
    read_audio(environment, options);

    return previous.mode != options->mode ||
           previous.layout != options->layout ||
           previous.link_enabled != options->link_enabled ||
           previous.swap_players != options->swap_players ||
           previous.audio_player1 != options->audio_player1;
}
