#include "asset_extractor.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <zstd.h>

namespace
{
using ChaChaState = std::array<std::byte, 64>;
using namespace std::string_view_literals;

constexpr std::array known_filepaths{
    "Data/Fonts/fontdebug.fnb"sv,
    "Data/Fonts/fontfirasans.fnb"sv,
    "Data/Fonts/fontmono.fnb"sv,
    "Data/Fonts/fontnewrodin.fnb"sv,
    "Data/Fonts/fontnotosansboldcs.fnb"sv,
    "Data/Fonts/fontnotosansboldct.fnb"sv,
    "Data/Fonts/fontnotosansboldko.fnb"sv,
    "Data/Fonts/fontnotosansboldnicknames.fnb"sv,
    "Data/Fonts/fontnotosansboldru.fnb"sv,
    "Data/Fonts/fontnotosansitaliccs.fnb"sv,
    "Data/Fonts/fontnotosansitalicct.fnb"sv,
    "Data/Fonts/fontnotosansitalicko.fnb"sv,
    "Data/Fonts/fontnotosansitalicru.fnb"sv,
    "Data/Fonts/fontrodincattleya.fnb"sv,
    "Data/Fonts/fontyorkten.fnb"sv,
    "Data/Levels/abzu.lvl"sv,
    "Data/Levels/Arena/dm1-1.lvl"sv,
    "Data/Levels/Arena/dm1-2.lvl"sv,
    "Data/Levels/Arena/dm1-3.lvl"sv,
    "Data/Levels/Arena/dm1-4.lvl"sv,
    "Data/Levels/Arena/dm1-5.lvl"sv,
    "Data/Levels/Arena/dm2-1.lvl"sv,
    "Data/Levels/Arena/dm2-2.lvl"sv,
    "Data/Levels/Arena/dm2-3.lvl"sv,
    "Data/Levels/Arena/dm2-4.lvl"sv,
    "Data/Levels/Arena/dm2-5.lvl"sv,
    "Data/Levels/Arena/dm3-1.lvl"sv,
    "Data/Levels/Arena/dm3-2.lvl"sv,
    "Data/Levels/Arena/dm3-3.lvl"sv,
    "Data/Levels/Arena/dm3-4.lvl"sv,
    "Data/Levels/Arena/dm3-5.lvl"sv,
    "Data/Levels/Arena/dm4-1.lvl"sv,
    "Data/Levels/Arena/dm4-2.lvl"sv,
    "Data/Levels/Arena/dm4-3.lvl"sv,
    "Data/Levels/Arena/dm4-4.lvl"sv,
    "Data/Levels/Arena/dm4-5.lvl"sv,
    "Data/Levels/Arena/dm5-1.lvl"sv,
    "Data/Levels/Arena/dm5-2.lvl"sv,
    "Data/Levels/Arena/dm5-3.lvl"sv,
    "Data/Levels/Arena/dm5-4.lvl"sv,
    "Data/Levels/Arena/dm5-5.lvl"sv,
    "Data/Levels/Arena/dm6-1.lvl"sv,
    "Data/Levels/Arena/dm6-2.lvl"sv,
    "Data/Levels/Arena/dm6-3.lvl"sv,
    "Data/Levels/Arena/dm6-4.lvl"sv,
    "Data/Levels/Arena/dm6-5.lvl"sv,
    "Data/Levels/Arena/dm7-1.lvl"sv,
    "Data/Levels/Arena/dm7-2.lvl"sv,
    "Data/Levels/Arena/dm7-3.lvl"sv,
    "Data/Levels/Arena/dm7-4.lvl"sv,
    "Data/Levels/Arena/dm7-5.lvl"sv,
    "Data/Levels/Arena/dm8-1.lvl"sv,
    "Data/Levels/Arena/dm8-2.lvl"sv,
    "Data/Levels/Arena/dm8-3.lvl"sv,
    "Data/Levels/Arena/dm8-4.lvl"sv,
    "Data/Levels/Arena/dm8-5.lvl"sv,
    "Data/Levels/Arena/dmpreview.tok"sv,
    "Data/Levels/babylonarea_1-1.lvl"sv,
    "Data/Levels/babylonarea.lvl"sv,
    "Data/Levels/basecamp_garden.lvl"sv,
    "Data/Levels/basecamp_shortcut_discovered.lvl"sv,
    "Data/Levels/basecamp_shortcut_undiscovered.lvl"sv,
    "Data/Levels/basecamp_shortcut_unlocked.lvl"sv,
    "Data/Levels/basecamp_surface.lvl"sv,
    "Data/Levels/basecamp_tutorial.lvl"sv,
    "Data/Levels/basecamp_tv_room_locked.lvl"sv,
    "Data/Levels/basecamp_tv_room_unlocked.lvl"sv,
    "Data/Levels/basecamp.lvl"sv,
    "Data/Levels/beehive.lvl"sv,
    "Data/Levels/blackmarket.lvl"sv,
    "Data/Levels/cavebossarea.lvl"sv,
    "Data/Levels/challenge_moon.lvl"sv,
    "Data/Levels/challenge_star.lvl"sv,
    "Data/Levels/challenge_sun.lvl"sv,
    "Data/Levels/cityofgold.lvl"sv,
    "Data/Levels/cosmicocean_babylon.lvl"sv,
    "Data/Levels/cosmicocean_dwelling.lvl"sv,
    "Data/Levels/cosmicocean_icecavesarea.lvl"sv,
    "Data/Levels/cosmicocean_jungle.lvl"sv,
    "Data/Levels/cosmicocean_sunkencity.lvl"sv,
    "Data/Levels/cosmicocean_temple.lvl"sv,
    "Data/Levels/cosmicocean_tidepool.lvl"sv,
    "Data/Levels/cosmicocean_volcano.lvl"sv,
    "Data/Levels/duat.lvl"sv,
    "Data/Levels/dwellingarea.lvl"sv,
    "Data/Levels/eggplantarea.lvl"sv,
    "Data/Levels/ending_hard.lvl"sv,
    "Data/Levels/ending.lvl"sv,
    "Data/Levels/generic.lvl"sv,
    "Data/Levels/hallofushabti.lvl"sv,
    "Data/Levels/hundun.lvl"sv,
    "Data/Levels/icecavesarea.lvl"sv,
    "Data/Levels/junglearea.lvl"sv,
    "Data/Levels/lake.lvl"sv,
    "Data/Levels/lakeoffire.lvl"sv,
    "Data/Levels/olmecarea.lvl"sv,
    "Data/Levels/palaceofpleasure.lvl"sv,
    "Data/Levels/sunkencityarea.lvl"sv,
    "Data/Levels/templearea.lvl"sv,
    "Data/Levels/testingarea.lvl"sv,
    "Data/Levels/tiamat.lvl"sv,
    "Data/Levels/tidepoolarea.lvl"sv,
    "Data/Levels/vladscastle.lvl"sv,
    "Data/Levels/volcanoarea.lvl"sv,
    "Data/Textures/base_eggship.DDS"sv,
    "Data/Textures/base_eggship.png"sv,
    "Data/Textures/base_eggship2.DDS"sv,
    "Data/Textures/base_eggship2.png"sv,
    "Data/Textures/base_eggship3.DDS"sv,
    "Data/Textures/base_eggship3.png"sv,
    "Data/Textures/base_skynight.DDS"sv,
    "Data/Textures/base_skynight.png"sv,
    "Data/Textures/base_surface.DDS"sv,
    "Data/Textures/base_surface.png"sv,
    "Data/Textures/base_surface2.DDS"sv,
    "Data/Textures/base_surface2.png"sv,
    "Data/Textures/bayer8.DDS"sv,
    "Data/Textures/bayer8.png"sv,
    "Data/Textures/bg_babylon.DDS"sv,
    "Data/Textures/bg_babylon.png"sv,
    "Data/Textures/bg_beehive.DDS"sv,
    "Data/Textures/bg_beehive.png"sv,
    "Data/Textures/bg_cave.DDS"sv,
    "Data/Textures/bg_cave.png"sv,
    "Data/Textures/bg_duat.DDS"sv,
    "Data/Textures/bg_duat.png"sv,
    "Data/Textures/bg_duat2.DDS"sv,
    "Data/Textures/bg_duat2.png"sv,
    "Data/Textures/bg_eggplant.DDS"sv,
    "Data/Textures/bg_eggplant.png"sv,
    "Data/Textures/bg_gold.DDS"sv,
    "Data/Textures/bg_gold.png"sv,
    "Data/Textures/bg_ice.DDS"sv,
    "Data/Textures/bg_ice.png"sv,
    "Data/Textures/bg_jungle.DDS"sv,
    "Data/Textures/bg_jungle.png"sv,
    "Data/Textures/bg_mothership.DDS"sv,
    "Data/Textures/bg_mothership.png"sv,
    "Data/Textures/bg_stone.DDS"sv,
    "Data/Textures/bg_stone.png"sv,
    "Data/Textures/bg_sunken.DDS"sv,
    "Data/Textures/bg_sunken.png"sv,
    "Data/Textures/bg_temple.DDS"sv,
    "Data/Textures/bg_temple.png"sv,
    "Data/Textures/bg_tidepool.DDS"sv,
    "Data/Textures/bg_tidepool.png"sv,
    "Data/Textures/bg_vlad.DDS"sv,
    "Data/Textures/bg_vlad.png"sv,
    "Data/Textures/bg_volcano.DDS"sv,
    "Data/Textures/bg_volcano.png"sv,
    "Data/Textures/border_main.DDS"sv,
    "Data/Textures/border_main.png"sv,
    "Data/Textures/char_black.DDS"sv,
    "Data/Textures/char_black.png"sv,
    "Data/Textures/char_blue.DDS"sv,
    "Data/Textures/char_blue.png"sv,
    "Data/Textures/char_cerulean.DDS"sv,
    "Data/Textures/char_cerulean.png"sv,
    "Data/Textures/char_cinnabar.DDS"sv,
    "Data/Textures/char_cinnabar.png"sv,
    "Data/Textures/char_cyan.DDS"sv,
    "Data/Textures/char_cyan.png"sv,
    "Data/Textures/char_eggchild.DDS"sv,
    "Data/Textures/char_eggchild.png"sv,
    "Data/Textures/char_gold.DDS"sv,
    "Data/Textures/char_gold.png"sv,
    "Data/Textures/char_gray.DDS"sv,
    "Data/Textures/char_gray.png"sv,
    "Data/Textures/char_green.DDS"sv,
    "Data/Textures/char_green.png"sv,
    "Data/Textures/char_hired.DDS"sv,
    "Data/Textures/char_hired.png"sv,
    "Data/Textures/char_iris.DDS"sv,
    "Data/Textures/char_iris.png"sv,
    "Data/Textures/char_khaki.DDS"sv,
    "Data/Textures/char_khaki.png"sv,
    "Data/Textures/char_lemon.DDS"sv,
    "Data/Textures/char_lemon.png"sv,
    "Data/Textures/char_lime.DDS"sv,
    "Data/Textures/char_lime.png"sv,
    "Data/Textures/char_magenta.DDS"sv,
    "Data/Textures/char_magenta.png"sv,
    "Data/Textures/char_olive.DDS"sv,
    "Data/Textures/char_olive.png"sv,
    "Data/Textures/char_orange.DDS"sv,
    "Data/Textures/char_orange.png"sv,
    "Data/Textures/char_pink.DDS"sv,
    "Data/Textures/char_pink.png"sv,
    "Data/Textures/char_red.DDS"sv,
    "Data/Textures/char_red.png"sv,
    "Data/Textures/char_violet.DDS"sv,
    "Data/Textures/char_violet.png"sv,
    "Data/Textures/char_white.DDS"sv,
    "Data/Textures/char_white.png"sv,
    "Data/Textures/char_yellow.DDS"sv,
    "Data/Textures/char_yellow.png"sv,
    "Data/Textures/coffins.DDS"sv,
    "Data/Textures/coffins.png"sv,
    "Data/Textures/credits.DDS"sv,
    "Data/Textures/credits.png"sv,
    "Data/Textures/deco_babylon.DDS"sv,
    "Data/Textures/deco_babylon.png"sv,
    "Data/Textures/deco_basecamp.DDS"sv,
    "Data/Textures/deco_basecamp.png"sv,
    "Data/Textures/deco_cave.DDS"sv,
    "Data/Textures/deco_cave.png"sv,
    "Data/Textures/deco_cosmic.DDS"sv,
    "Data/Textures/deco_cosmic.png"sv,
    "Data/Textures/deco_eggplant.DDS"sv,
    "Data/Textures/deco_eggplant.png"sv,
    "Data/Textures/deco_extra.DDS"sv,
    "Data/Textures/deco_extra.png"sv,
    "Data/Textures/deco_gold.DDS"sv,
    "Data/Textures/deco_gold.png"sv,
    "Data/Textures/deco_ice.DDS"sv,
    "Data/Textures/deco_ice.png"sv,
    "Data/Textures/deco_jungle.DDS"sv,
    "Data/Textures/deco_jungle.png"sv,
    "Data/Textures/deco_sunken.DDS"sv,
    "Data/Textures/deco_sunken.png"sv,
    "Data/Textures/deco_temple.DDS"sv,
    "Data/Textures/deco_temple.png"sv,
    "Data/Textures/deco_tidepool.DDS"sv,
    "Data/Textures/deco_tidepool.png"sv,
    "Data/Textures/deco_tutorial.DDS"sv,
    "Data/Textures/deco_tutorial.png"sv,
    "Data/Textures/deco_volcano.DDS"sv,
    "Data/Textures/deco_volcano.png"sv,
    "Data/Textures/floor_babylon.DDS"sv,
    "Data/Textures/floor_babylon.png"sv,
    "Data/Textures/floor_cave.DDS"sv,
    "Data/Textures/floor_cave.png"sv,
    "Data/Textures/floor_eggplant.DDS"sv,
    "Data/Textures/floor_eggplant.png"sv,
    "Data/Textures/floor_ice.DDS"sv,
    "Data/Textures/floor_ice.png"sv,
    "Data/Textures/floor_jungle.DDS"sv,
    "Data/Textures/floor_jungle.png"sv,
    "Data/Textures/floor_sunken.DDS"sv,
    "Data/Textures/floor_sunken.png"sv,
    "Data/Textures/floor_surface.DDS"sv,
    "Data/Textures/floor_surface.png"sv,
    "Data/Textures/floor_temple.DDS"sv,
    "Data/Textures/floor_temple.png"sv,
    "Data/Textures/floor_tidepool.DDS"sv,
    "Data/Textures/floor_tidepool.png"sv,
    "Data/Textures/floor_volcano.DDS"sv,
    "Data/Textures/floor_volcano.png"sv,
    "Data/Textures/floormisc.DDS"sv,
    "Data/Textures/floormisc.png"sv,
    "Data/Textures/floorstyled_babylon.DDS"sv,
    "Data/Textures/floorstyled_babylon.png"sv,
    "Data/Textures/floorstyled_beehive.DDS"sv,
    "Data/Textures/floorstyled_beehive.png"sv,
    "Data/Textures/floorstyled_duat.DDS"sv,
    "Data/Textures/floorstyled_duat.png"sv,
    "Data/Textures/floorstyled_gold_normal.DDS"sv,
    "Data/Textures/floorstyled_gold_normal.png"sv,
    "Data/Textures/floorstyled_gold.DDS"sv,
    "Data/Textures/floorstyled_gold.png"sv,
    "Data/Textures/floorstyled_guts.DDS"sv,
    "Data/Textures/floorstyled_guts.png"sv,
    "Data/Textures/floorstyled_mothership.DDS"sv,
    "Data/Textures/floorstyled_mothership.png"sv,
    "Data/Textures/floorstyled_pagoda.DDS"sv,
    "Data/Textures/floorstyled_pagoda.png"sv,
    "Data/Textures/floorstyled_palace.DDS"sv,
    "Data/Textures/floorstyled_palace.png"sv,
    "Data/Textures/floorstyled_stone.DDS"sv,
    "Data/Textures/floorstyled_stone.png"sv,
    "Data/Textures/floorstyled_sunken.DDS"sv,
    "Data/Textures/floorstyled_sunken.png"sv,
    "Data/Textures/floorstyled_temple.DDS"sv,
    "Data/Textures/floorstyled_temple.png"sv,
    "Data/Textures/floorstyled_vlad.DDS"sv,
    "Data/Textures/floorstyled_vlad.png"sv,
    "Data/Textures/floorstyled_wood.DDS"sv,
    "Data/Textures/floorstyled_wood.png"sv,
    "Data/Textures/fontdebug.DDS"sv,
    "Data/Textures/fontdebug.png"sv,
    "Data/Textures/fontfirasans.DDS"sv,
    "Data/Textures/fontfirasans.png"sv,
    "Data/Textures/fontmono.DDS"sv,
    "Data/Textures/fontmono.png"sv,
    "Data/Textures/fontnewrodin.DDS"sv,
    "Data/Textures/fontnewrodin.png"sv,
    "Data/Textures/fontnotosansboldcs_0.DDS"sv,
    "Data/Textures/fontnotosansboldct_0.DDS"sv,
    "Data/Textures/fontnotosansboldko_0.DDS"sv,
    "Data/Textures/fontnotosansboldnicknames_0.DDS"sv,
    "Data/Textures/fontnotosansboldru_0.DDS"sv,
    "Data/Textures/fontnotosansitaliccs_0.DDS"sv,
    "Data/Textures/fontnotosansitalicct_0.DDS"sv,
    "Data/Textures/fontnotosansitalicko_0.DDS"sv,
    "Data/Textures/fontnotosansitalicru_0.DDS"sv,
    "Data/Textures/fontrodincattleya.DDS"sv,
    "Data/Textures/fontrodincattleya.png"sv,
    "Data/Textures/fontyorkten.DDS"sv,
    "Data/Textures/fontyorkten.png"sv,
    "Data/Textures/fx_ankh.DDS"sv,
    "Data/Textures/fx_ankh.png"sv,
    "Data/Textures/fx_big.DDS"sv,
    "Data/Textures/fx_big.png"sv,
    "Data/Textures/fx_explosion.DDS"sv,
    "Data/Textures/fx_explosion.png"sv,
    "Data/Textures/fx_rubble.DDS"sv,
    "Data/Textures/fx_rubble.png"sv,
    "Data/Textures/fx_small.DDS"sv,
    "Data/Textures/fx_small.png"sv,
    "Data/Textures/fx_small2.DDS"sv,
    "Data/Textures/fx_small2.png"sv,
    "Data/Textures/fx_small3.DDS"sv,
    "Data/Textures/fx_small3.png"sv,
    "Data/Textures/hud_controller_buttons.DDS"sv,
    "Data/Textures/hud_controller_buttons.png"sv,
    "Data/Textures/hud_text.DDS"sv,
    "Data/Textures/hud_text.png"sv,
    "Data/Textures/hud.DDS"sv,
    "Data/Textures/hud.png"sv,
    "Data/Textures/items_ushabti.DDS"sv,
    "Data/Textures/items_ushabti.png"sv,
    "Data/Textures/items.DDS"sv,
    "Data/Textures/items.png"sv,
    "Data/Textures/journal_back.DDS"sv,
    "Data/Textures/journal_back.png"sv,
    "Data/Textures/journal_elements.DDS"sv,
    "Data/Textures/journal_elements.png"sv,
    "Data/Textures/journal_entry_bg.DDS"sv,
    "Data/Textures/journal_entry_bg.png"sv,
    "Data/Textures/journal_entry_items.DDS"sv,
    "Data/Textures/journal_entry_items.png"sv,
    "Data/Textures/journal_entry_mons_big.DDS"sv,
    "Data/Textures/journal_entry_mons_big.png"sv,
    "Data/Textures/journal_entry_mons.DDS"sv,
    "Data/Textures/journal_entry_mons.png"sv,
    "Data/Textures/journal_entry_people.DDS"sv,
    "Data/Textures/journal_entry_people.png"sv,
    "Data/Textures/journal_entry_place.DDS"sv,
    "Data/Textures/journal_entry_place.png"sv,
    "Data/Textures/journal_entry_traps.DDS"sv,
    "Data/Textures/journal_entry_traps.png"sv,
    "Data/Textures/journal_pageflip.DDS"sv,
    "Data/Textures/journal_pageflip.png"sv,
    "Data/Textures/journal_pagetorn.DDS"sv,
    "Data/Textures/journal_pagetorn.png"sv,
    "Data/Textures/journal_select.DDS"sv,
    "Data/Textures/journal_select.png"sv,
    "Data/Textures/journal_stickers.DDS"sv,
    "Data/Textures/journal_stickers.png"sv,
    "Data/Textures/journal_story.DDS"sv,
    "Data/Textures/journal_story.png"sv,
    "Data/Textures/journal_top_entry.DDS"sv,
    "Data/Textures/journal_top_entry.png"sv,
    "Data/Textures/journal_top_gameover.DDS"sv,
    "Data/Textures/journal_top_gameover.png"sv,
    "Data/Textures/journal_top_main.DDS"sv,
    "Data/Textures/journal_top_main.png"sv,
    "Data/Textures/journal_top_profile.DDS"sv,
    "Data/Textures/journal_top_profile.png"sv,
    "Data/Textures/journal_trophies.DDS"sv,
    "Data/Textures/loading.DDS"sv,
    "Data/Textures/loading.png"sv,
    "Data/Textures/lut_backlayer.DDS"sv,
    "Data/Textures/lut_backlayer.png"sv,
    "Data/Textures/lut_blackmarket.DDS"sv,
    "Data/Textures/lut_blackmarket.png"sv,
    "Data/Textures/lut_icecaves.DDS"sv,
    "Data/Textures/lut_icecaves.png"sv,
    "Data/Textures/lut_original.DDS"sv,
    "Data/Textures/lut_original.png"sv,
    "Data/Textures/lut_vlad.DDS"sv,
    "Data/Textures/lut_vlad.png"sv,
    "Data/Textures/main_body.DDS"sv,
    "Data/Textures/main_body.png"sv,
    "Data/Textures/main_dirt.DDS"sv,
    "Data/Textures/main_dirt.png"sv,
    "Data/Textures/main_door.DDS"sv,
    "Data/Textures/main_door.png"sv,
    "Data/Textures/main_doorback.DDS"sv,
    "Data/Textures/main_doorback.png"sv,
    "Data/Textures/main_doorframe.DDS"sv,
    "Data/Textures/main_doorframe.png"sv,
    "Data/Textures/main_fore1.DDS"sv,
    "Data/Textures/main_fore1.png"sv,
    "Data/Textures/main_fore2.DDS"sv,
    "Data/Textures/main_fore2.png"sv,
    "Data/Textures/main_head.DDS"sv,
    "Data/Textures/main_head.png"sv,
    "Data/Textures/menu_basic.DDS"sv,
    "Data/Textures/menu_basic.png"sv,
    "Data/Textures/menu_brick1.DDS"sv,
    "Data/Textures/menu_brick1.png"sv,
    "Data/Textures/menu_brick2.DDS"sv,
    "Data/Textures/menu_brick2.png"sv,
    "Data/Textures/menu_cave1.DDS"sv,
    "Data/Textures/menu_cave1.png"sv,
    "Data/Textures/menu_cave2.DDS"sv,
    "Data/Textures/menu_cave2.png"sv,
    "Data/Textures/menu_chardoor.DDS"sv,
    "Data/Textures/menu_chardoor.png"sv,
    "Data/Textures/menu_charsel.DDS"sv,
    "Data/Textures/menu_charsel.png"sv,
    "Data/Textures/menu_deathmatch.DDS"sv,
    "Data/Textures/menu_deathmatch.png"sv,
    "Data/Textures/menu_deathmatch2.DDS"sv,
    "Data/Textures/menu_deathmatch2.png"sv,
    "Data/Textures/menu_deathmatch3.DDS"sv,
    "Data/Textures/menu_deathmatch3.png"sv,
    "Data/Textures/menu_deathmatch4.DDS"sv,
    "Data/Textures/menu_deathmatch4.png"sv,
    "Data/Textures/menu_deathmatch5.DDS"sv,
    "Data/Textures/menu_deathmatch5.png"sv,
    "Data/Textures/menu_deathmatch6.DDS"sv,
    "Data/Textures/menu_deathmatch6.png"sv,
    "Data/Textures/menu_disp.DDS"sv,
    "Data/Textures/menu_disp.png"sv,
    "Data/Textures/menu_generic.DDS"sv,
    "Data/Textures/menu_generic.png"sv,
    "Data/Textures/menu_header.DDS"sv,
    "Data/Textures/menu_header.png"sv,
    "Data/Textures/menu_leader.DDS"sv,
    "Data/Textures/menu_leader.png"sv,
    "Data/Textures/menu_online.DDS"sv,
    "Data/Textures/menu_online.png"sv,
    "Data/Textures/menu_title.DDS"sv,
    "Data/Textures/menu_title.png"sv,
    "Data/Textures/menu_titlegal.DDS"sv,
    "Data/Textures/menu_titlegal.png"sv,
    "Data/Textures/menu_tunnel.DDS"sv,
    "Data/Textures/menu_tunnel.png"sv,
    "Data/Textures/monsters_ghost.DDS"sv,
    "Data/Textures/monsters_ghost.png"sv,
    "Data/Textures/monsters_hundun.DDS"sv,
    "Data/Textures/monsters_hundun.png"sv,
    "Data/Textures/monsters_olmec.DDS"sv,
    "Data/Textures/monsters_olmec.png"sv,
    "Data/Textures/monsters_osiris.DDS"sv,
    "Data/Textures/monsters_osiris.png"sv,
    "Data/Textures/monsters_pets.DDS"sv,
    "Data/Textures/monsters_pets.png"sv,
    "Data/Textures/monsters_tiamat.DDS"sv,
    "Data/Textures/monsters_tiamat.png"sv,
    "Data/Textures/monsters_yama.DDS"sv,
    "Data/Textures/monsters_yama.png"sv,
    "Data/Textures/monsters01.DDS"sv,
    "Data/Textures/monsters01.png"sv,
    "Data/Textures/monsters02.DDS"sv,
    "Data/Textures/monsters02.png"sv,
    "Data/Textures/monsters03.DDS"sv,
    "Data/Textures/monsters03.png"sv,
    "Data/Textures/monstersbasic01.DDS"sv,
    "Data/Textures/monstersbasic01.png"sv,
    "Data/Textures/monstersbasic02.DDS"sv,
    "Data/Textures/monstersbasic02.png"sv,
    "Data/Textures/monstersbasic03.DDS"sv,
    "Data/Textures/monstersbasic03.png"sv,
    "Data/Textures/monstersbig01.DDS"sv,
    "Data/Textures/monstersbig01.png"sv,
    "Data/Textures/monstersbig02.DDS"sv,
    "Data/Textures/monstersbig02.png"sv,
    "Data/Textures/monstersbig03.DDS"sv,
    "Data/Textures/monstersbig03.png"sv,
    "Data/Textures/monstersbig04.DDS"sv,
    "Data/Textures/monstersbig04.png"sv,
    "Data/Textures/monstersbig05.DDS"sv,
    "Data/Textures/monstersbig05.png"sv,
    "Data/Textures/monstersbig06.DDS"sv,
    "Data/Textures/monstersbig06.png"sv,
    "Data/Textures/mounts.DDS"sv,
    "Data/Textures/mounts.png"sv,
    "Data/Textures/noise0.DDS"sv,
    "Data/Textures/noise0.png"sv,
    "Data/Textures/noise1.DDS"sv,
    "Data/Textures/noise1.png"sv,
    "Data/Textures/OldTextures/ai.DDS"sv,
    "Data/Textures/OldTextures/ai.png"sv,
    "Data/Textures/saving.DDS"sv,
    "Data/Textures/saving.png"sv,
    "Data/Textures/shadows.DDS"sv,
    "Data/Textures/shadows.png"sv,
    "Data/Textures/shine.DDS"sv,
    "Data/Textures/shine.png"sv,
    "Data/Textures/splash0.DDS"sv,
    "Data/Textures/splash0.png"sv,
    "Data/Textures/splash1.DDS"sv,
    "Data/Textures/splash1.png"sv,
    "Data/Textures/splash2.DDS"sv,
    "Data/Textures/splash2.png"sv,
    "shaders.hlsl"sv,
    "soundbank.bank"sv,
    "soundbank.strings.bank"sv,
    "strings00.str"sv,
    "strings01.str"sv,
    "strings02.str"sv,
    "strings03.str"sv,
    "strings04.str"sv,
    "strings05.str"sv,
    "strings06.str"sv,
    "strings07.str"sv,
    "strings08.str"sv,
    "strings09.str"sv,
    "strings10.str"sv,
    "strings11.str"sv,
    "strings12.str"sv,
};

uint64_t read_u64(const ChaChaState& state, size_t index)
{
    uint64_t value{};
    for (size_t byte = 0; byte < sizeof(value); ++byte)
    {
        value |= static_cast<uint64_t>(std::to_integer<uint8_t>(state[index * sizeof(value) + byte])) << (byte * 8);
    }
    return value;
}

uint32_t read_u32(const ChaChaState& state, size_t index)
{
    uint32_t value{};
    for (size_t byte = 0; byte < sizeof(value); ++byte)
    {
        value |= static_cast<uint32_t>(std::to_integer<uint8_t>(state[index * sizeof(value) + byte])) << (byte * 8);
    }
    return value;
}

void write_u64(ChaChaState& state, size_t index, uint64_t value)
{
    for (size_t byte = 0; byte < sizeof(value); ++byte)
    {
        state[index * sizeof(value) + byte] = static_cast<std::byte>((value >> (byte * 8)) & 0xff);
    }
}

void write_u32(ChaChaState& state, size_t index, uint32_t value)
{
    for (size_t byte = 0; byte < sizeof(value); ++byte)
    {
        state[index * sizeof(value) + byte] = static_cast<std::byte>((value >> (byte * 8)) & 0xff);
    }
}

void quarter_round(std::array<uint32_t, 16>& words, size_t a, size_t b, size_t c, size_t d)
{
    words[a] += words[b];
    words[d] = std::rotl(words[d] ^ words[a], 16);
    words[c] += words[d];
    words[b] = std::rotl(words[b] ^ words[c], 12);
    words[a] += words[b];
    words[d] = std::rotl(words[d] ^ words[a], 8);
    words[c] += words[d];
    words[b] = std::rotl(words[b] ^ words[c], 7);
}

void round_pair(std::array<uint32_t, 16>& words)
{
    quarter_round(words, 0, 4, 8, 12);
    quarter_round(words, 1, 5, 9, 13);
    quarter_round(words, 2, 6, 10, 14);
    quarter_round(words, 3, 7, 11, 15);
    quarter_round(words, 0, 5, 10, 15);
    quarter_round(words, 1, 6, 11, 12);
    quarter_round(words, 2, 7, 8, 13);
    quarter_round(words, 3, 4, 9, 14);
}

ChaChaState run_rounds(ChaChaState state, size_t pair_count)
{
    std::array<uint32_t, 16> words{};
    for (size_t index = 0; index < words.size(); ++index)
    {
        words[index] = read_u32(state, index);
    }
    for (size_t pair = 0; pair < pair_count; ++pair)
    {
        round_pair(words);
    }
    for (size_t index = 0; index < words.size(); ++index)
    {
        write_u32(state, index, words[index]);
    }
    return state;
}

ChaChaState make_state(const std::array<uint64_t, 8>& words)
{
    ChaChaState state{};
    for (size_t index = 0; index < words.size(); ++index)
    {
        write_u64(state, index, words[index]);
    }
    return state;
}

ChaChaState add_states_as_u64(const ChaChaState& first, const ChaChaState& second)
{
    ChaChaState result{};
    for (size_t index = 0; index < 8; ++index)
    {
        write_u64(result, index, read_u64(first, index) + read_u64(second, index));
    }
    return result;
}

ChaChaState mix_path(ChaChaState state, std::string_view path)
{
    for (size_t offset = 0; offset < path.size(); offset += state.size())
    {
        const size_t count{ std::min(state.size(), path.size() - offset) };
        for (size_t index = 0; index < count; ++index)
        {
            state[index] ^= static_cast<std::byte>(static_cast<uint8_t>(path[offset + count - index - 1]));
        }
        state = run_rounds(state, 4);
    }
    return state;
}

std::vector<std::byte> hash_filepath(uint64_t key, std::string_view path)
{
    ChaChaState state{ run_rounds(make_state({ key, path.size(), 0, 0, 0, 0, 0, 0 }), 2) };
    state = mix_path(state, path);
    state = add_states_as_u64(state, run_rounds(state, 4));
    std::array<uint64_t, 8> words{};
    for (size_t index = 0; index < words.size(); ++index)
    {
        words[index] = read_u64(state, index);
    }
    words[0] ^= path.size();
    state = run_rounds(make_state(words), 4);

    std::vector<std::byte> result(path.size());
    for (size_t offset = 0; offset < path.size(); offset += state.size())
    {
        const size_t count{ std::min(state.size(), path.size() - offset) };
        for (size_t index = 0; index < count; ++index)
        {
            result[offset + index] = static_cast<std::byte>(static_cast<uint8_t>(path[offset + index])) ^ state[count - index - 1];
        }
    }
    return result;
}

std::vector<std::byte> decrypt_asset(uint64_t key, std::string_view path, std::span<const std::byte> data)
{
    ChaChaState state{ run_rounds(make_state({ key, path.size(), 0, 0, 0, 0, 0, 0 }), 2) };
    state = mix_path(state, path);
    state = add_states_as_u64(state, run_rounds(state, 4));
    std::array<uint64_t, 8> words{};
    for (size_t index = 0; index < words.size(); ++index)
    {
        words[index] = read_u64(state, index);
    }
    words[0] ^= key + data.size();
    state = run_rounds(make_state(words), 4);

    std::vector<std::byte> result(data.size());
    const size_t full_size{ data.size() / state.size() * state.size() };
    for (size_t index = 0; index < full_size; ++index)
    {
        result[index] = data[index] ^ state[state.size() - index % state.size() - 1];
    }
    const size_t tail_size{ data.size() - full_size };
    for (size_t index = 0; index < tail_size; ++index)
    {
        result[full_size + index] = data[full_size + index] ^ state[tail_size - index - 1];
    }
    return result;
}

uint64_t update_nasam_key(uint64_t key, uint64_t data)
{
    data ^= key;
    data ^= std::rotr(data, 25) ^ std::rotr(data, 47);
    data *= 0x9E6C63D0676A9A99;
    data ^= data >> 23 ^ data >> 51;
    data *= 0x9E6D62D06F6A9A9B;
    data ^= data >> 23 ^ data >> 51;
    return data ^ key;
}

struct AssetRecord
{
    size_t bundle_offset{};
    size_t record_size{};
    std::vector<std::byte> filepath_hash;
    std::span<const std::byte> data;
    bool encrypted{};
};

struct ParsedBundle
{
    std::vector<AssetRecord> records;
    size_t record_bytes{};
    size_t inter_record_padding_bytes{};
    size_t trailing_padding_bytes{};
    size_t terminator_bytes{};
    size_t unclassified_bytes{};
};

uint32_t read_u32_le(std::span<const std::byte> data, size_t offset)
{
    uint32_t value{};
    for (size_t byte = 0; byte < sizeof(value); ++byte)
    {
        value |= static_cast<uint32_t>(std::to_integer<uint8_t>(data[offset + byte])) << (byte * 8);
    }
    return value;
}

bool is_all_zero(std::span<const std::byte> data)
{
    return std::all_of(data.begin(), data.end(), [](std::byte value)
                       { return value == std::byte{}; });
}

bool has_valid_record_header(std::span<const std::byte> bundle_data, size_t offset)
{
    if (bundle_data.size() - offset < 8)
    {
        return false;
    }

    const uint32_t data_length{ read_u32_le(bundle_data, offset) };
    const uint32_t filepath_length{ read_u32_le(bundle_data, offset + 4) };
    const size_t remaining{ bundle_data.size() - offset - 8 };
    if (data_length == 0 || filepath_length == 0 || filepath_length > remaining ||
        data_length > remaining - filepath_length)
    {
        return false;
    }

    const std::byte encryption_flag{ bundle_data[offset + 8 + filepath_length] };
    return encryption_flag == std::byte{} || encryption_flag == std::byte{ 1 };
}

size_t find_next_record_after_zero_padding(std::span<const std::byte> bundle_data, size_t offset)
{
    constexpr size_t maximum_padding_size{ 0x1000 };
    const size_t maximum_scan{ std::min(maximum_padding_size, bundle_data.size() - offset) };
    for (size_t padding_size = 1; padding_size <= maximum_scan; ++padding_size)
    {
        if (bundle_data[offset + padding_size - 1] != std::byte{})
        {
            break;
        }
        if (has_valid_record_header(bundle_data, offset + padding_size))
        {
            return offset + padding_size;
        }
    }
    return std::string_view::npos;
}

ParsedBundle parse_bundle(std::span<const std::byte> bundle_data, uint64_t& key)
{
    ParsedBundle parsed;
    size_t offset{};
    while (offset < bundle_data.size())
    {
        if (bundle_data.size() - offset < 8)
        {
            const auto remainder{ bundle_data.subspan(offset) };
            if (is_all_zero(remainder))
            {
                parsed.trailing_padding_bytes += remainder.size();
            }
            else
            {
                parsed.unclassified_bytes += remainder.size();
            }
            break;
        }
        const uint32_t data_length{ read_u32_le(bundle_data, offset) };
        const uint32_t filepath_length{ read_u32_le(bundle_data, offset + 4) };

        if (data_length == 0 && filepath_length == 0)
        {
            const size_t next_record{ find_next_record_after_zero_padding(bundle_data, offset) };
            if (next_record != std::string_view::npos)
            {
                parsed.inter_record_padding_bytes += next_record - offset;
                offset = next_record;
                continue;
            }

            parsed.terminator_bytes += 8;
            offset += 8;
            const auto remainder{ bundle_data.subspan(offset) };
            size_t zero_padding{};
            while (zero_padding < remainder.size() && remainder[zero_padding] == std::byte{})
            {
                ++zero_padding;
            }
            parsed.trailing_padding_bytes += zero_padding;
            parsed.unclassified_bytes += remainder.size() - zero_padding;
            break;
        }
        if (!has_valid_record_header(bundle_data, offset))
        {
            const size_t next_record{ find_next_record_after_zero_padding(bundle_data, offset) };
            if (next_record != std::string_view::npos)
            {
                parsed.inter_record_padding_bytes += next_record - offset;
                offset = next_record;
                continue;
            }

            const auto remainder{ bundle_data.subspan(offset) };
            if (is_all_zero(remainder))
            {
                parsed.trailing_padding_bytes += remainder.size();
            }
            else
            {
                parsed.unclassified_bytes += remainder.size();
            }
            break;
        }

        const size_t record_start{ offset };
        offset += 8;
        AssetRecord record;
        record.bundle_offset = record_start;
        record.filepath_hash.assign(bundle_data.begin() + offset, bundle_data.begin() + offset + filepath_length);
        offset += filepath_length;
        record.encrypted = bundle_data[offset] == std::byte{ 1 };
        ++offset;
        const size_t payload_length{ data_length - 1 };
        record.data = bundle_data.subspan(offset, payload_length);
        offset += payload_length;
        record.record_size = offset - record_start;
        parsed.record_bytes += record.record_size;
        key = update_nasam_key(key, data_length);
        parsed.records.push_back(std::move(record));
    }
    return parsed;
}

std::unordered_map<std::string, std::string_view> make_filepath_registry(uint64_t key)
{
    std::unordered_map<std::string, std::string_view> registry;
    for (const auto& path : known_filepaths)
    {
        const std::vector<std::byte> hash{ hash_filepath(key, path) };
        registry.emplace(std::string(reinterpret_cast<const char*>(hash.data()), hash.size()), path);
    }
    return registry;
}

std::string_view find_filepath(const AssetRecord& record,
                               const std::unordered_map<std::string, std::string_view>& registry)
{
    std::string key(reinterpret_cast<const char*>(record.filepath_hash.data()), record.filepath_hash.size());
    if (const auto found = registry.find(key); found != registry.end())
    {
        return found->second;
    }
    while (!key.empty() && key.back() == '\0')
    {
        key.pop_back();
    }
    if (const auto found = registry.find(key); found != registry.end())
    {
        return found->second;
    }
    return {};
}

std::string format_filepath_hash(std::span<const std::byte> hash)
{
    constexpr char hex_digits[]{ "0123456789abcdef" };
    std::string result;
    result.reserve(hash.size() * 2);
    for (const std::byte value : hash)
    {
        const uint8_t byte{ std::to_integer<uint8_t>(value) };
        result.push_back(hex_digits[byte >> 4]);
        result.push_back(hex_digits[byte & 0x0f]);
    }
    return result;
}

struct ZstdStreamDeleter
{
    void operator()(ZSTD_DStream* stream) const
    {
        ZSTD_freeDStream(stream);
    }
};

std::vector<std::byte> decompress_zstd(std::span<const std::byte> data)
{
    std::unique_ptr<ZSTD_DStream, ZstdStreamDeleter> stream{ ZSTD_createDStream() };
    if (!stream)
    {
        throw std::runtime_error("Could not create zstd decompression stream");
    }
    size_t status{ ZSTD_initDStream(stream.get()) };
    if (ZSTD_isError(status))
    {
        throw std::runtime_error(ZSTD_getErrorName(status));
    }

    ZSTD_inBuffer input{ data.data(), data.size(), 0 };
    std::vector<std::byte> chunk(ZSTD_DStreamOutSize());
    std::vector<std::byte> result;
    while (input.pos < input.size || status != 0)
    {
        ZSTD_outBuffer output{ chunk.data(), chunk.size(), 0 };
        status = ZSTD_decompressStream(stream.get(), &output, &input);
        if (ZSTD_isError(status))
        {
            throw std::runtime_error(ZSTD_getErrorName(status));
        }
        result.insert(result.end(), chunk.begin(), chunk.begin() + output.pos);
        if (input.pos == input.size && output.pos == 0 && status != 0)
        {
            throw std::runtime_error("Truncated zstd asset data");
        }
    }
    return result;
}

void write_asset(const std::filesystem::path& output_directory,
                 std::string_view filepath,
                 std::span<const std::byte> data)
{
    const std::filesystem::path relative_path{ filepath };
    if (relative_path.is_absolute() || relative_path.has_root_name())
    {
        throw std::runtime_error("Asset path is not relative");
    }
    for (const auto& component : relative_path)
    {
        if (component == "..")
        {
            throw std::runtime_error("Asset path escapes the output directory");
        }
    }

    const std::filesystem::path output_path{ output_directory / relative_path };
    std::filesystem::create_directories(output_path.parent_path());
    std::ofstream file(output_path, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        throw std::runtime_error("Could not create asset file: " + output_path.string());
    }
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!file)
    {
        throw std::runtime_error("Could not write asset file: " + output_path.string());
    }
}
} // namespace

AssetExtractionReport ExtractAssetBundle(std::span<const std::byte> bundle_data,
                                         const std::filesystem::path& output_directory,
                                         const AssetExtractionProgress& on_progress)
{
    AssetExtractionReport report;
    report.bundle_bytes = bundle_data.size();
    uint64_t key{};
    ParsedBundle parsed{ parse_bundle(bundle_data, key) };
    report.record_count = parsed.records.size();
    report.record_bytes = parsed.record_bytes;
    report.inter_record_padding_bytes = parsed.inter_record_padding_bytes;
    report.trailing_padding_bytes = parsed.trailing_padding_bytes;
    report.terminator_bytes = parsed.terminator_bytes;
    report.unclassified_bytes = parsed.unclassified_bytes;
    const auto registry{ make_filepath_registry(key) };
    for (const AssetRecord& record : parsed.records)
    {
        if (!find_filepath(record, registry).empty())
        {
            ++report.matched_asset_count;
        }
        else
        {
            ++report.unrecognized_record_count;
            report.unrecognized_record_bytes += record.record_size;
            report.unrecognized_assets.push_back({
                record.bundle_offset,
                record.record_size,
                record.data.size(),
                record.encrypted,
                format_filepath_hash(record.filepath_hash),
            });
        }
    }

    for (const AssetRecord& record : parsed.records)
    {
        const std::string_view filepath{ find_filepath(record, registry) };
        if (filepath.empty())
        {
            continue;
        }

        std::vector<std::byte> decoded;
        if (record.encrypted)
        {
            decoded = decrypt_asset(key, filepath, record.data);
            decoded = decompress_zstd(decoded);
        }
        else
        {
            decoded.assign(record.data.begin(), record.data.end());
        }

        write_asset(output_directory, filepath, decoded);
        ++report.extracted_asset_count;
        report.extracted_file_bytes += decoded.size();
        if (on_progress)
        {
            on_progress(report.extracted_asset_count, report.matched_asset_count);
        }
    }
    return report;
}