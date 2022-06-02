#include "ReceiveSubcommands.hh"

#include <string.h>

#include <memory>
#include <phosg/Strings.hh>

#include "Client.hh"
#include "Lobby.hh"
#include "Player.hh"
#include "PSOProtocol.hh"
#include "SendCommands.hh"
#include "Text.hh"
#include "Items.hh"

using namespace std;

// The functions in this file are called when a BB client sends a game command
// (60, 62, 6C, or 6D) that must be handled by the server.



struct ItemSubcommand {
  uint8_t command;
  uint8_t size;
  uint8_t client_id;
  uint8_t unused;
  uint32_t item_id;
  uint32_t amount;
};



void check_size(uint16_t size, uint16_t min_size, uint16_t max_size) {
  if (size < min_size) {
    throw runtime_error(string_printf(
        "command too small (expected at least 0x%hX bytes, got 0x%hX bytes)",
        min_size, size));
  }
  if (max_size == 0) {
    max_size = min_size;
  }
  if (size > max_size) {
    throw runtime_error(string_printf(
        "command too large (expected at most 0x%hX bytes, got 0x%hX bytes)",
        max_size, size));
  }
}



bool command_is_private(uint8_t command) {
  // TODO: are either of the ep3 commands private? looks like not
  return (command == 0x62) || (command == 0x6D);
}



void forward_subcommand(shared_ptr<Lobby> l, shared_ptr<Client> c,
    uint8_t command, uint8_t flag, const PSOSubcommand* p,
    size_t count) {

  // if the command is an Ep3-only command, make sure an Ep3 client sent it
  bool command_is_ep3 = (command & 0xF0) == 0xC0;
  if (command_is_ep3 && !(c->flags & ClientFlag::EPISODE_3_GAMES)) {
    return;
  }

  if (command_is_private(command)) {
    if (flag >= l->max_clients) {
      return;
    }
    auto target = l->clients[flag];
    if (!target) {
      return;
    }
    if (command_is_ep3 && !(target->flags & ClientFlag::EPISODE_3_GAMES)) {
      return;
    }
    send_command(target, command, flag, p, count * 4);

  } else {
    if (command_is_ep3) {
      for (auto& target : l->clients) {
        if (!target || (target == c) || !(target->flags & ClientFlag::EPISODE_3_GAMES)) {
          continue;
        }
        send_command(target, command, flag, p, count * 4);
      }

    } else {
      send_command_excluding_client(l, c, command, flag, p, count * 4);
    }
  }
}



////////////////////////////////////////////////////////////////////////////////
// Chat commands and the like

// client requests to send a guild card
static void process_subcommand_send_guild_card(shared_ptr<ServerState>,
    shared_ptr<Lobby> l, shared_ptr<Client> c, uint8_t command, uint8_t flag,
    const PSOSubcommand* p, size_t count) {
  check_size(count, 9, 0xFFFF);

  if (!command_is_private(command) || !l || flag >= l->max_clients ||
      (!l->clients[flag]) || (p->byte[1] != count)) {
    return;
  }

  if (c->version == GameVersion::GC) {
    if (count < 0x25) {
      return;
    }
    decode_sjis(c->player.guild_card_desc,
        reinterpret_cast<const char*>(&p[9].byte[0]), 0x58);
  }

  send_guild_card(l->clients[flag], c);
}

// client sends a symbol chat
static void process_subcommand_symbol_chat(shared_ptr<ServerState>,
    shared_ptr<Lobby> l, shared_ptr<Client> c, uint8_t command, uint8_t flag,
    const PSOSubcommand* p, size_t count) {
  check_size(count, 2, 0xFFFF);

  if (!c->can_chat || (p->byte[1] != count) || (p->byte[1] < 2) ||
      (p[1].byte[0] != c->lobby_client_id)) {
    return;
  }
  forward_subcommand(l, c, command, flag, p, count);
}

// client sends a word select chat
static void process_subcommand_word_select(shared_ptr<ServerState>,
    shared_ptr<Lobby> l, shared_ptr<Client> c, uint8_t command, uint8_t flag,
    const PSOSubcommand* p, size_t count) {
  check_size(count, 8, 0xFFFF);

  if (!c->can_chat || (p->byte[1] != count) || (p->byte[1] < 8) ||
      (p->byte[2] != c->lobby_client_id)) {
    return;
  }

  // TODO: bring this back if it turns out to be important; I suspect it's not
  //p->byte[2] = p->byte[3] = p->byte[(c->version == GameVersion::BB) ? 2 : 3];

  for (size_t x = 1; x < 8; x++) {
    if ((p[x].word[0] > 0x1863) && (p[x].word[0] != 0xFFFF)) {
      return;
    }
    if ((p[x].word[1] > 0x1863) && (p[x].word[1] != 0xFFFF)) {
      return;
    }
  }
  forward_subcommand(l, c, command, flag, p, count);
}

////////////////////////////////////////////////////////////////////////////////
// Game commands used by cheat mechanisms

// need to process changing areas since we keep track of where players are
static void process_subcommand_change_area(shared_ptr<ServerState>,
    shared_ptr<Lobby> l, shared_ptr<Client> c, uint8_t command, uint8_t flag,
    const PSOSubcommand* p, size_t count) {
  check_size(count, 2, 0xFFFF);
  if (!l->is_game() || (p->byte[1] != count)) {
    return;
  }
  c->area = p[1].dword;
  forward_subcommand(l, c, command, flag, p, count);
}

// when a player is hit by a monster, heal them if infinite HP is enabled
static void process_subcommand_hit_by_monster(shared_ptr<ServerState>,
    shared_ptr<Lobby> l, shared_ptr<Client> c, uint8_t command, uint8_t flag,
    const PSOSubcommand* p, size_t count) {
  if (!l->is_game() || (p->byte[2] != c->lobby_client_id)) {
    return;
  }
  forward_subcommand(l, c, command, flag, p, count);
  if ((l->flags & LobbyFlag::CHEATS_ENABLED) && c->infinite_hp) {
    send_player_stats_change(l, c, PlayerStatsChange::ADD_HP, 1020);
  }
}

// when a player casts a tech, restore TP if infinite TP is enabled
static void process_subcommand_use_technique(shared_ptr<ServerState>,
    shared_ptr<Lobby> l, shared_ptr<Client> c, uint8_t command, uint8_t flag,
    const PSOSubcommand* p, size_t count) {
  if (!l->is_game() || (p->byte[1] != count) || (p->byte[2] != c->lobby_client_id)) {
    return;
  }
  forward_subcommand(l, c, command, flag, p, count);
  if ((l->flags & LobbyFlag::CHEATS_ENABLED) && c->infinite_tp) {
    send_player_stats_change(l, c, PlayerStatsChange::ADD_TP, 255);
  }
}

////////////////////////////////////////////////////////////////////////////////
// BB Item commands

// player drops an item
static void process_subcommand_drop_item(shared_ptr<ServerState>,
    shared_ptr<Lobby> l, shared_ptr<Client> c, uint8_t command, uint8_t flag,
    const PSOSubcommand* p, size_t count) {
  if (l->version == GameVersion::BB) {
    check_size(count, 6);

    struct Cmd {
      uint8_t command;
      uint8_t size;
      uint8_t client_id;
      uint8_t unused;
      uint16_t unused2; // should be 1
      uint16_t area;
      uint32_t item_id;
      float x;
      float y;
      float z;
    };
    auto* cmd = reinterpret_cast<const Cmd*>(p);

    if ((cmd->size != 6) || (cmd->client_id != c->lobby_client_id)) {
      return;
    }

    PlayerInventoryItem item;
    c->player.remove_item(cmd->item_id, 0, &item);
    l->add_item(item);
  }
  forward_subcommand(l, c, command, flag, p, count);
}

// player splits a stack and drops part of it
static void process_subcommand_drop_stacked_item(shared_ptr<ServerState>,
    shared_ptr<Lobby> l, shared_ptr<Client> c, uint8_t command, uint8_t flag,
    const PSOSubcommand* p, size_t count) {
  if (l->version == GameVersion::BB) {
    check_size(count, 6);

    struct Cmd {
      uint8_t command;
      uint8_t size;
      uint8_t client_id;
      uint8_t unused;
      uint16_t area;
      uint16_t unused2;
      float x;
      float y;
      uint32_t item_id;
      uint32_t amount;
    };
    auto* cmd = reinterpret_cast<const Cmd*>(p);

    if (!l->is_game() || (cmd->size != 6) || (cmd->client_id != c->lobby_client_id)) {
      return;
    }

    PlayerInventoryItem item;
    c->player.remove_item(cmd->item_id, cmd->amount, &item);

    // if a stack was split, the original item still exists, so the dropped item
    // needs a new ID. remove_item signals this by returning an item with id=-1
    if (item.data.item_id == 0xFFFFFFFF) {
      item.data.item_id = l->generate_item_id(c->lobby_client_id);
    }

    l->add_item(item);

    send_drop_stacked_item(l, item.data, cmd->area, cmd->x, cmd->y);

  } else {
    forward_subcommand(l, c, command, flag, p, count);
  }
}

// player requests to pick up an item
static void process_subcommand_pick_up_item(shared_ptr<ServerState>,
    shared_ptr<Lobby> l, shared_ptr<Client> c, uint8_t command, uint8_t flag,
    const PSOSubcommand* p, size_t count) {
  if (l->version == GameVersion::BB) {
    check_size(count, 3);

    struct Cmd {
      uint8_t command;
      uint8_t size;
      uint8_t client_id;
      uint8_t unused;
      uint32_t item_id;
      uint8_t area;
      uint8_t unused2[3];
    };
    auto* cmd = reinterpret_cast<const Cmd*>(p);

    if (!l->is_game() || (cmd->size != 3) || (cmd->client_id != c->lobby_client_id)) {
      return;
    }

    PlayerInventoryItem item;
    l->remove_item(cmd->item_id, &item);
    c->player.add_item(item);

    send_pick_up_item(l, c, item.data.item_id, cmd->area);

  } else {
    forward_subcommand(l, c, command, flag, p, count);
  }
}

// player equips an item
static void process_subcommand_equip_unequip_item(shared_ptr<ServerState>,
    shared_ptr<Lobby> l, shared_ptr<Client> c, uint8_t command, uint8_t flag,
    const PSOSubcommand* p, size_t count) {
  if (l->version == GameVersion::BB) {
    check_size(count, 3);

    auto* cmd = reinterpret_cast<const ItemSubcommand*>(p);
    if ((cmd->size != 3) || (cmd->client_id != c->lobby_client_id)) {
      return;
    }

    size_t index = c->player.inventory.find_item(cmd->item_id);
    if (cmd->command == 0x25) {
      c->player.inventory.items[index].game_flags |= 0x00000008; // equip
    } else {
      c->player.inventory.items[index].game_flags &= 0xFFFFFFF7; // unequip
    }

  } else {
    forward_subcommand(l, c, command, flag, p, count);
  }
}

static void process_subcommand_use_item(shared_ptr<ServerState>,
    shared_ptr<Lobby> l, shared_ptr<Client> c, uint8_t command, uint8_t flag,
    const PSOSubcommand* p, size_t count) {
  if (l->version == GameVersion::BB) {
    check_size(count, 2);

    auto* cmd = reinterpret_cast<const ItemSubcommand*>(p);
    if ((cmd->size != 2) || (cmd->client_id != c->lobby_client_id)) {
      return;
    }

    size_t index = c->player.inventory.find_item(cmd->item_id);
    if (cmd->command == 0x25) {
      c->player.inventory.items[index].game_flags |= 0x00000008; // equip
    } else {
      c->player.inventory.items[index].game_flags &= 0xFFFFFFF7; // unequip
    }

    player_use_item(c, index);
  }

  forward_subcommand(l, c, command, flag, p, count);
}

static void process_subcommand_open_shop_or_ep3_unknown(shared_ptr<ServerState> s,
    shared_ptr<Lobby> l, shared_ptr<Client> c, uint8_t command, uint8_t flag,
    const PSOSubcommand* p, size_t count) {
  if (l->flags & LobbyFlag::EPISODE_3) {
    check_size(count, 2, 0xFFFF);
    forward_subcommand(l, c, command, flag, p, count);

  } else {
    uint32_t shop_type = p[1].dword;

    if ((l->version == GameVersion::BB) && l->is_game()) {
      size_t num_items = (rand() % 4) + 9;
      c->player.current_shop_contents.clear();
      while (c->player.current_shop_contents.size() < num_items) {
        ItemData item_data;
        if (shop_type == 0) { // tool shop
          item_data = s->common_item_creator->create_shop_item(l->difficulty, 3);
        } else if (shop_type == 1) { // weapon shop
          item_data = s->common_item_creator->create_shop_item(l->difficulty, 0);
        } else if (shop_type == 2) { // guards shop
          item_data = s->common_item_creator->create_shop_item(l->difficulty, 1);
        } else { // unknown shop... just leave it blank I guess
          break;
        }

        item_data.item_id = l->generate_item_id(c->lobby_client_id);
        c->player.current_shop_contents.emplace_back(item_data);
      }

      send_shop(c, shop_type);
    }
  }
}

static void process_subcommand_open_bank(shared_ptr<ServerState>,
    shared_ptr<Lobby> l, shared_ptr<Client> c, uint8_t, uint8_t,
    const PSOSubcommand*, size_t) {
  if ((l->version == GameVersion::BB) && l->is_game()) {
    send_bank(c);
  }
}

// player performs some bank action
static void process_subcommand_bank_action(shared_ptr<ServerState>,
    shared_ptr<Lobby> l, shared_ptr<Client> c, uint8_t, uint8_t,
    const PSOSubcommand* p, size_t count) {
  if (l->version == GameVersion::BB) {
    check_size(count, 4);

    struct Cmd {
      uint8_t subcommand;
      uint8_t size;
      uint16_t unused;
      uint32_t item_id;
      uint32_t meseta_amount;
      uint8_t action;
      uint8_t item_amount;
      uint16_t unused2;
    };
    auto* cmd = reinterpret_cast<const Cmd*>(p);

    if (!l->is_game() || (cmd->size != 4)) {
      return;
    }

    if (cmd->action == 0) { // deposit
      if (cmd->item_id == 0xFFFFFFFF) { // meseta
        if (cmd->meseta_amount > c->player.disp.meseta) {
          return;
        }
        if ((c->player.bank.meseta + cmd->meseta_amount) > 999999) {
          return;
        }
        c->player.bank.meseta += cmd->meseta_amount;
        c->player.disp.meseta -= cmd->meseta_amount;
      } else { // item
        PlayerInventoryItem item;
        c->player.remove_item(cmd->item_id, cmd->item_amount, &item);
        c->player.bank.add_item(item.to_bank_item());
        send_destroy_item(l, c, cmd->item_id, cmd->item_amount);
      }
    } else if (cmd->action == 1) { // take
      if (cmd->item_id == 0xFFFFFFFF) { // meseta
        if (cmd->meseta_amount > c->player.bank.meseta) {
          return;
        }
        if ((c->player.disp.meseta + cmd->meseta_amount) > 999999) {
          return;
        }
        c->player.bank.meseta -= cmd->meseta_amount;
        c->player.disp.meseta += cmd->meseta_amount;
      } else { // item
        PlayerBankItem bank_item;
        c->player.bank.remove_item(cmd->item_id, cmd->item_amount, &bank_item);
        PlayerInventoryItem item = bank_item.to_inventory_item();
        item.data.item_id = l->generate_item_id(0xFF);
        c->player.add_item(item);
        send_create_inventory_item(l, c, item.data);
      }
    }
  }
}

// player sorts the items in their inventory
static void process_subcommand_sort_inventory(shared_ptr<ServerState>,
    shared_ptr<Lobby> l, shared_ptr<Client> c, uint8_t, uint8_t,
    const PSOSubcommand* p, size_t count) {
  if (l->version == GameVersion::BB) {
    check_size(count, 31);

    struct Cmd {
      uint8_t command;
      uint8_t size;
      uint16_t unused;
      uint32_t item_ids[30];
    };
    auto* cmd = reinterpret_cast<const Cmd*>(p);

    if (cmd->size != 31) {
      return;
    }

    PlayerInventory sorted;
    memset(&sorted, 0, sizeof(PlayerInventory));

    for (size_t x = 0; x < 30; x++) {
      if (cmd->item_ids[x] == 0xFFFFFFFF) {
        sorted.items[x].data.item_id = 0xFFFFFFFF;
      } else {
        size_t index = c->player.inventory.find_item(cmd->item_ids[x]);
        sorted.items[x] = c->player.inventory.items[index];
      }
    }

    sorted.num_items = c->player.inventory.num_items;
    sorted.hp_materials_used = c->player.inventory.hp_materials_used;
    sorted.tp_materials_used = c->player.inventory.tp_materials_used;
    sorted.language = c->player.inventory.language;
    c->player.inventory = sorted;
  }
}

////////////////////////////////////////////////////////////////////////////////
// BB EXP/Drop Item commands

// enemy killed; leader sends drop item request
static void process_subcommand_enemy_drop_item(shared_ptr<ServerState> s,
    shared_ptr<Lobby> l, shared_ptr<Client> c, uint8_t command, uint8_t flag,
    const PSOSubcommand* p, size_t count) {
  if (l->version == GameVersion::BB) {
    check_size(count, 6);

    struct Cmd {
      uint8_t command;
      uint8_t size;
      uint16_t unused;
      uint8_t area;
      uint8_t monster_id;
      uint16_t request_id;
      float x;
      float y;
      uint32_t unknown[2];
    };
    auto* cmd = reinterpret_cast<const Cmd*>(p);

    if ((cmd->size != 6) || !l->is_game()) {
      return;
    }

    PlayerInventoryItem item;
    memset(&item, 0, sizeof(PlayerInventoryItem));

    bool is_rare = false;
    if (l->next_drop_item.data.item_data1d[0]) {
      item = l->next_drop_item;
      l->next_drop_item.data.item_data1d[0] = 0;
    } else {
      if (l->rare_item_set) {
        if (cmd->monster_id <= 0x65) {
          is_rare = sample_rare_item(l->rare_item_set->rares[cmd->monster_id].probability);
        }
      }

      if (is_rare) {
        memcpy(&item.data.item_data1d, l->rare_item_set->rares[cmd->monster_id].item_code, 3);
        //RandPercentages();
        if (item.data.item_data1d[0] == 0) {
          item.data.item_data1[4] |= 0x80; // make it untekked if it's a weapon
        }
      } else {
        try {
          item.data = s->common_item_creator->create_drop_item(false, l->episode,
              l->difficulty, cmd->area, l->section_id);
        } catch (const out_of_range&) {
          // create_common_item throws this when it doesn't want to make an item
          return;
        }
      }
    }
    item.data.item_id = l->generate_item_id(0xFF);

    l->add_item(item);
    send_drop_item(l, item.data, false, cmd->area, cmd->x, cmd->y,
        cmd->request_id);

  } else {
    forward_subcommand(l, c, command, flag, p, count);
  }
}

// box broken; leader sends drop item request
static void process_subcommand_box_drop_item(shared_ptr<ServerState> s,
    shared_ptr<Lobby> l, shared_ptr<Client> c, uint8_t command, uint8_t flag,
    const PSOSubcommand* p, size_t count) {
  if (l->version == GameVersion::BB) {
    check_size(count, 10);

    struct Cmd {
      uint8_t command;
      uint8_t size;
      uint16_t unused;
      uint8_t area;
      uint8_t unused2;
      uint16_t request_id;
      float x;
      float y;
      uint32_t unknown[6];
    };
    auto* cmd = reinterpret_cast<const Cmd*>(p);

    if ((cmd->size != 10) || !l->is_game()) {
      return;
    }

    PlayerInventoryItem item;
    memset(&item, 0, sizeof(PlayerInventoryItem));

    bool is_rare = false;
    if (l->next_drop_item.data.item_data1d[0]) {
      item = l->next_drop_item;
      l->next_drop_item.data.item_data1d[0] = 0;
    } else {
      size_t index;
      if (l->rare_item_set) {
        for (index = 0; index < 30; index++) {
          if (l->rare_item_set->box_areas[index] != cmd->area) {
            continue;
          }
          if (sample_rare_item(l->rare_item_set->box_rares[index].probability)) {
            is_rare = true;
            break;
          }
        }
      }

      if (is_rare) {
        memcpy(item.data.item_data1, l->rare_item_set->box_rares[index].item_code, 3);
        //RandPercentages();
        if (item.data.item_data1d[0] == 0) {
          item.data.item_data1[4] |= 0x80; // make it untekked if it's a weapon
        }
      } else {
        try {
          item.data = s->common_item_creator->create_drop_item(true, l->episode,
              l->difficulty, cmd->area, l->section_id);
        } catch (const out_of_range&) {
          // create_common_item throws this when it doesn't want to make an item
          return;
        }
      }
    }
    item.data.item_id = l->generate_item_id(0xFF);

    l->add_item(item);
    send_drop_item(l, item.data, false, cmd->area, cmd->x, cmd->y,
        cmd->request_id);

  } else {
    forward_subcommand(l, c, command, flag, p, count);
  }
}

// monster hit by player
static void process_subcommand_monster_hit(shared_ptr<ServerState>,
    shared_ptr<Lobby> l, shared_ptr<Client> c, uint8_t command, uint8_t flag,
    const PSOSubcommand* p, size_t count) {
  if (l->version == GameVersion::BB) {
    check_size(count, 10);

    struct Cmd {
      uint8_t command;
      uint8_t size;
      uint16_t enemy_id2;
      uint16_t enemy_id;
      uint16_t damage;
      uint32_t flags;
    };
    auto* cmd = reinterpret_cast<const Cmd*>(p);

    if (cmd->size != 10) {
      return;
    }

    if (cmd->enemy_id >= l->enemies.size()) {
      return;
    }

    if (l->enemies[cmd->enemy_id].hit_flags & 0x80) {
      return;
    }
    l->enemies[cmd->enemy_id].hit_flags |= (1 << c->lobby_client_id);
    l->enemies[cmd->enemy_id].last_hit = c->lobby_client_id;
  }

  forward_subcommand(l, c, command, flag, p, count);
}

// monster killed by player
static void process_subcommand_monster_killed(shared_ptr<ServerState> s,
    shared_ptr<Lobby> l, shared_ptr<Client> c, uint8_t command, uint8_t flag,
    const PSOSubcommand* p, size_t count) {
  if (l->version == GameVersion::BB) {
    check_size(count, 3);
  }

  forward_subcommand(l, c, command, flag, p, count);

  if (l->version == GameVersion::BB) {
    struct Cmd {
      uint8_t command;
      uint8_t size;
      uint16_t enemy_id2;
      uint16_t enemy_id;
      uint16_t killer_client_id;
      uint32_t unused;
    };
    auto* cmd = reinterpret_cast<const Cmd*>(p);

    if (!l->is_game() || (cmd->size != 3) || (cmd->enemy_id >= l->enemies.size() ||
        (l->enemies[cmd->enemy_id].hit_flags & 0x80))) {
      return;
    }

    if (l->enemies[cmd->enemy_id].experience == 0xFFFFFFFF) {
      send_text_message(c, u"$C6Unknown enemy type killed");
      return;
    }

    auto& enemy = l->enemies[cmd->enemy_id];
    enemy.hit_flags |= 0x80;
    for (size_t x = 0; x < l->max_clients; x++) {
      if (!((enemy.hit_flags >> x) & 1)) {
        continue; // player did not hit this enemy
      }

      auto other_c = l->clients[x];
      if (!other_c) {
        continue; // no player
      }
      if (other_c->player.disp.level >= 199) {
        continue; // player is level 200 or higher
      }

      // killer gets full experience, others get 77%
      uint32_t exp;
      if (enemy.last_hit == other_c->lobby_client_id) {
        exp = enemy.experience;
      } else {
        exp = ((enemy.experience * 77) / 100);
      }

      other_c->player.disp.experience += exp;
      send_give_experience(l, other_c, exp);

      bool leveled_up = false;
      do {
        const auto& level = s->level_table->stats_for_level(
            other_c->player.disp.char_class, other_c->player.disp.level + 1);
        if (other_c->player.disp.experience >= level.experience) {
          leveled_up = true;
          level.apply(other_c->player.disp.stats);
          other_c->player.disp.level++;
        } else {
          break;
        }
      } while (other_c->player.disp.level < 199);
      if (leveled_up) {
        send_level_up(l, other_c);
      }
    }
  }
}

// destroy item (sent when there are too many items on the ground)
static void process_subcommand_destroy_item(shared_ptr<ServerState>,
    shared_ptr<Lobby> l, shared_ptr<Client> c, uint8_t command, uint8_t flag,
    const PSOSubcommand* p, size_t count) {
  if (l->version == GameVersion::BB) {
    check_size(count, 3);

    auto* cmd = reinterpret_cast<const ItemSubcommand*>(p);
    if ((cmd->size != 3) || !l->is_game()) {
      return;
    }
    l->remove_item(cmd->item_id, nullptr);
  }

  forward_subcommand(l, c, command, flag, p, count);
}

// player requests to tekk an item
static void process_subcommand_identify_item(shared_ptr<ServerState>,
    shared_ptr<Lobby> l, shared_ptr<Client> c, uint8_t command, uint8_t flag,
    const PSOSubcommand* p, size_t count) {
  if (l->version == GameVersion::BB) {
    check_size(count, 3);

    auto* cmd = reinterpret_cast<const ItemSubcommand*>(p);
    if (!l->is_game() || (cmd->size != 3) || (cmd->client_id != c->lobby_client_id)) {
      return;
    }

    size_t x = c->player.inventory.find_item(cmd->item_id);
    if (c->player.inventory.items[x].data.item_data1[0] != 0) {
      return; // only weapons can be identified
    }

    c->player.disp.meseta -= 100;
    c->player.identify_result = c->player.inventory.items[x];
    c->player.identify_result.data.item_data1[4] &= 0x7F;

    // TODO: move this into a SendCommands.cc
    PSOSubcommand sub[6];
    sub[0].byte[0] = 0xB9;
    sub[0].byte[1] = 0x06;
    sub[0].word[1] = c->lobby_client_id;
    memcpy(&sub[1], &c->player.identify_result.data, sizeof(ItemData));
    send_command(l, 0x60, 0x00, sub, 0x18);

  } else {
    forward_subcommand(l, c, command, flag, p, count);
  }
}

// player accepts the tekk
// TODO: I don't know which subcommand id this is; the function should be
// correct though so we can just put it in the table when we figure out the id
// static void process_subcommand_accept_identified_item(shared_ptr<ServerState> s,
//     shared_ptr<Lobby> l, shared_ptr<Client> c, uint8_t command, uint8_t flag,
//     const PSOSubcommand* p, size_t count) {
//
//   if (l->version == GameVersion::BB) {
//     check_size(count, 3);
//
//     auto* cmd = reinterpret_cast<const ItemSubcommand*>(p);
//     if ((cmd->size != 3) || (cmd->client_id != c->lobby_client_id)) {
//       return;
//     }
//
//     size_t x = c->player.inventory.find_item(cmd->item_id);
//     c->player.inventory.items[x] = c->player.identify_result;
//     // TODO: what do we send to the other clients? anything?
//
//   } else {
//     forward_subcommand(l, c, command, flag, p, count);
//   }
// }

////////////////////////////////////////////////////////////////////////////////

static void process_subcommand_forward_check_size(shared_ptr<ServerState>,
    shared_ptr<Lobby> l, shared_ptr<Client> c, uint8_t command, uint8_t flag,
    const PSOSubcommand* p, size_t count) {
  if (p->byte[1] != count) {
    return;
  }
  forward_subcommand(l, c, command, flag, p, count);
}

static void process_subcommand_forward_check_game(shared_ptr<ServerState>,
    shared_ptr<Lobby> l, shared_ptr<Client> c, uint8_t command, uint8_t flag,
    const PSOSubcommand* p, size_t count) {
  if (!l->is_game()) {
    return;
  }
  forward_subcommand(l, c, command, flag, p, count);
}

static void process_subcommand_forward_check_game_loading(shared_ptr<ServerState>,
    shared_ptr<Lobby> l, shared_ptr<Client> c, uint8_t command, uint8_t flag,
    const PSOSubcommand* p, size_t count) {
  if (!l->is_game() || !l->any_client_loading()) {
    return;
  }
  forward_subcommand(l, c, command, flag, p, count);
}

static void process_subcommand_forward_check_size_client(shared_ptr<ServerState>,
    shared_ptr<Lobby> l, shared_ptr<Client> c, uint8_t command, uint8_t flag,
    const PSOSubcommand* p, size_t count) {
  if ((p->byte[1] != count) || (p->byte[2] != c->lobby_client_id)) {
    return;
  }
  forward_subcommand(l, c, command, flag, p, count);
}

static void process_subcommand_forward_check_size_game(shared_ptr<ServerState>,
    shared_ptr<Lobby> l, shared_ptr<Client> c, uint8_t command, uint8_t flag,
    const PSOSubcommand* p, size_t count) {
  if (!l->is_game() || (p->byte[1] != count)) {
    return;
  }
  forward_subcommand(l, c, command, flag, p, count);
}

static void process_subcommand_forward_check_size_ep3_lobby(shared_ptr<ServerState>,
    shared_ptr<Lobby> l, shared_ptr<Client> c, uint8_t command, uint8_t flag,
    const PSOSubcommand* p, size_t count) {
  if (!(l->flags & LobbyFlag::EPISODE_3) || l->is_game() || (p->byte[1] != count)) {
    return;
  }
  forward_subcommand(l, c, command, flag, p, count);
}

static void process_subcommand_invalid(shared_ptr<ServerState>,
    shared_ptr<Lobby>, shared_ptr<Client>, uint8_t command, uint8_t flag,
    const PSOSubcommand* p, size_t count) {
  if (command_is_private(command)) {
    log(WARNING, "Invalid subcommand: %02hhX (%zu of them) (private to player %hhu)",
        p->byte[0], count, flag);
  } else {
    log(WARNING, "Invalid subcommand: %02hhX (%zu of them) (public)",
        p->byte[0], count);
  }
}

static void process_subcommand_unimplemented(shared_ptr<ServerState>,
    shared_ptr<Lobby>, shared_ptr<Client>, uint8_t command, uint8_t flag,
    const PSOSubcommand* p, size_t count) {
  if (command_is_private(command)) {
    log(WARNING, "Unknown subcommand: %02hhX (%zu of them) (private to player %hhu)",
        p->byte[0], count, flag);
  } else {
    log(WARNING, "Unknown subcommand: %02hhX (%zu of them) (public)",
        p->byte[0], count);
  }
}

////////////////////////////////////////////////////////////////////////////////

// Subcommands are described by four fields: the minimum size and maximum size (in DWORDs),
// the handler function, and flags that tell when to allow the command. See command-input-subs.h
// for more information on flags. The maximum size is not enforced if it's zero.
typedef void (*subcommand_handler_t)(shared_ptr<ServerState> s,
    shared_ptr<Lobby> l, shared_ptr<Client> c, uint8_t command, uint8_t flag,
    const PSOSubcommand* p, size_t count);

subcommand_handler_t subcommand_handlers[0x100] = {
  /* 00 */ process_subcommand_invalid,
  /* 01 */ process_subcommand_unimplemented,
  /* 02 */ process_subcommand_unimplemented,
  /* 03 */ process_subcommand_unimplemented,
  /* 04 */ process_subcommand_unimplemented,
  /* 05 */ process_subcommand_forward_check_size_game, // Switch flipped (door lock / lights / poison gas), or room unlocked when all enemied defeated
  /* 06 */ process_subcommand_send_guild_card,
  /* 07 */ process_subcommand_symbol_chat,
  /* 08 */ process_subcommand_unimplemented,
  /* 09 */ process_subcommand_unimplemented,
  /* 0A */ process_subcommand_monster_hit,
  /* 0B */ process_subcommand_forward_check_size_game, // Box destroyed
  /* 0C */ process_subcommand_forward_check_size_game, // Add condition (poison/slow/etc.)
  /* 0D */ process_subcommand_forward_check_size_game, // Remove condition (poison/slow/etc.)
  /* 0E */ process_subcommand_unimplemented,
  /* 0F */ process_subcommand_unimplemented,
  /* 10 */ process_subcommand_unimplemented,
  /* 11 */ process_subcommand_unimplemented,
  /* 12 */ process_subcommand_forward_check_size_game, // Dragon actions
  /* 13 */ process_subcommand_forward_check_size_game, // De Rol Le actions
  /* 14 */ process_subcommand_forward_check_size_game,
  /* 15 */ process_subcommand_forward_check_size_game, // Vol Opt actions
  /* 16 */ process_subcommand_forward_check_size_game, // Vol Opt actions
  /* 17 */ process_subcommand_forward_check_size_game,
  /* 18 */ process_subcommand_forward_check_size_game,
  /* 19 */ process_subcommand_forward_check_size_game, // Dark Falz actions
  /* 1A */ process_subcommand_unimplemented,
  /* 1B */ process_subcommand_unimplemented,
  /* 1C */ process_subcommand_forward_check_size_game,
  /* 1D */ process_subcommand_unimplemented,
  /* 1E */ process_subcommand_unimplemented,
  /* 1F */ process_subcommand_forward_check_size,
  /* 20 */ process_subcommand_forward_check_size,
  /* 21 */ process_subcommand_change_area, // Inter-level warp
  /* 22 */ process_subcommand_forward_check_size_client, // Set player visibility
  /* 23 */ process_subcommand_forward_check_size_client, // Set player visibility
  /* 24 */ process_subcommand_forward_check_size_game,
  /* 25 */ process_subcommand_equip_unequip_item, // Equip item
  /* 26 */ process_subcommand_equip_unequip_item, // Unequip item
  /* 27 */ process_subcommand_use_item,
  /* 28 */ process_subcommand_forward_check_size_game, // Feed MAG
  /* 29 */ process_subcommand_forward_check_size_game, // Delete item (via bank deposit / sale / feeding MAG)
  /* 2A */ process_subcommand_drop_item,
  /* 2B */ process_subcommand_forward_check_size_game,
  /* 2C */ process_subcommand_forward_check_size, // Talk to NPC
  /* 2D */ process_subcommand_forward_check_size, // Done talking to NPC
  /* 2E */ process_subcommand_unimplemented,
  /* 2F */ process_subcommand_hit_by_monster,
  /* 30 */ process_subcommand_forward_check_size_game, // Level up
  /* 31 */ process_subcommand_forward_check_size_game, // Medical center
  /* 32 */ process_subcommand_forward_check_size_game, // Medical center
  /* 33 */ process_subcommand_forward_check_size_game, // Revive player (only confirmed with moon atomizer)
  /* 34 */ process_subcommand_unimplemented,
  /* 35 */ process_subcommand_unimplemented,
  /* 36 */ process_subcommand_forward_check_game,
  /* 37 */ process_subcommand_forward_check_size_game, // Photon blast
  /* 38 */ process_subcommand_unimplemented,
  /* 39 */ process_subcommand_forward_check_size_game, // Photon blast ready
  /* 3A */ process_subcommand_forward_check_size_game,
  /* 3B */ process_subcommand_forward_check_size,
  /* 3C */ process_subcommand_unimplemented,
  /* 3D */ process_subcommand_unimplemented,
  /* 3E */ process_subcommand_forward_check_size, // Stop moving
  /* 3F */ process_subcommand_forward_check_size, // Set position
  /* 40 */ process_subcommand_forward_check_size, // Walk
  /* 41 */ process_subcommand_unimplemented,
  /* 42 */ process_subcommand_forward_check_size, // Run
  /* 43 */ process_subcommand_forward_check_size_client,
  /* 44 */ process_subcommand_forward_check_size_client,
  /* 45 */ process_subcommand_forward_check_size_client,
  /* 46 */ process_subcommand_forward_check_size_client,
  /* 47 */ process_subcommand_forward_check_size_client,
  /* 48 */ process_subcommand_use_technique,
  /* 49 */ process_subcommand_forward_check_size_client,
  /* 4A */ process_subcommand_forward_check_size_client,
  /* 4B */ process_subcommand_hit_by_monster,
  /* 4C */ process_subcommand_hit_by_monster,
  /* 4D */ process_subcommand_forward_check_size_client,
  /* 4E */ process_subcommand_forward_check_size_client,
  /* 4F */ process_subcommand_forward_check_size_client,
  /* 50 */ process_subcommand_forward_check_size_client,
  /* 51 */ process_subcommand_unimplemented,
  /* 52 */ process_subcommand_forward_check_size, // Toggle shop/bank interaction
  /* 53 */ process_subcommand_forward_check_size_game,
  /* 54 */ process_subcommand_unimplemented,
  /* 55 */ process_subcommand_forward_check_size_client, // Intra-map warp
  /* 56 */ process_subcommand_forward_check_size_client,
  /* 57 */ process_subcommand_forward_check_size_client,
  /* 58 */ process_subcommand_forward_check_size_game,
  /* 59 */ process_subcommand_forward_check_size_game, // Item picked up
  /* 5A */ process_subcommand_pick_up_item, // Request to pick up item
  /* 5B */ process_subcommand_unimplemented,
  /* 5C */ process_subcommand_unimplemented,
  /* 5D */ process_subcommand_forward_check_size_game, // Drop meseta or stacked item
  /* 5E */ process_subcommand_forward_check_size_game, // Buy item at shop
  /* 5F */ process_subcommand_forward_check_size_game, // Drop item from box/monster
  /* 60 */ process_subcommand_enemy_drop_item, // Request for item drop (handled by the server on BB)
  /* 61 */ process_subcommand_forward_check_size_game, // Feed mag
  /* 62 */ process_subcommand_unimplemented,
  /* 63 */ process_subcommand_destroy_item, // Destroy an item on the ground (used when too many items have been dropped)
  /* 64 */ process_subcommand_unimplemented,
  /* 65 */ process_subcommand_unimplemented,
  /* 66 */ process_subcommand_forward_check_size_game, // Star atomizer
  /* 67 */ process_subcommand_forward_check_size_game, // Create enemy set
  /* 68 */ process_subcommand_forward_check_size_game, // Telepipe/Ryuker
  /* 69 */ process_subcommand_forward_check_size_game,
  /* 6A */ process_subcommand_forward_check_size_game,
  /* 6B */ process_subcommand_forward_check_game_loading,
  /* 6C */ process_subcommand_forward_check_game_loading,
  /* 6D */ process_subcommand_forward_check_game_loading,
  /* 6E */ process_subcommand_forward_check_game_loading,
  /* 6F */ process_subcommand_forward_check_game_loading,
  /* 70 */ process_subcommand_forward_check_game_loading,
  /* 71 */ process_subcommand_forward_check_game_loading,
  /* 72 */ process_subcommand_forward_check_game_loading,
  /* 73 */ process_subcommand_invalid,
  /* 74 */ process_subcommand_word_select,
  /* 75 */ process_subcommand_forward_check_size_game,
  /* 76 */ process_subcommand_forward_check_size_game, // Monster killed
  /* 77 */ process_subcommand_forward_check_size_game, // Sync quest data
  /* 78 */ process_subcommand_unimplemented,
  /* 79 */ process_subcommand_forward_check_size, // Lobby 14/15 soccer game
  /* 7A */ process_subcommand_unimplemented,
  /* 7B */ process_subcommand_unimplemented,
  /* 7C */ process_subcommand_forward_check_size_game,
  /* 7D */ process_subcommand_forward_check_size_game,
  /* 7E */ process_subcommand_unimplemented,
  /* 7F */ process_subcommand_unimplemented,
  /* 80 */ process_subcommand_forward_check_size_game, // trigger trap
  /* 81 */ process_subcommand_unimplemented,
  /* 82 */ process_subcommand_unimplemented,
  /* 83 */ process_subcommand_forward_check_size_game, // place trap
  /* 84 */ process_subcommand_forward_check_size_game,
  /* 85 */ process_subcommand_forward_check_size_game,
  /* 86 */ process_subcommand_forward_check_size_game, // Hit destructible wall
  /* 87 */ process_subcommand_unimplemented,
  /* 88 */ process_subcommand_forward_check_size_game,
  /* 89 */ process_subcommand_forward_check_size_game,
  /* 8A */ process_subcommand_unimplemented,
  /* 8B */ process_subcommand_unimplemented,
  /* 8C */ process_subcommand_unimplemented,
  /* 8D */ process_subcommand_forward_check_size_client,
  /* 8E */ process_subcommand_unimplemented,
  /* 8F */ process_subcommand_unimplemented,
  /* 90 */ process_subcommand_unimplemented,
  /* 91 */ process_subcommand_forward_check_size_game,
  /* 92 */ process_subcommand_unimplemented,
  /* 93 */ process_subcommand_forward_check_size_game, // Timed switch activated
  /* 94 */ process_subcommand_forward_check_size_game, // Warp (the $warp chat command is implemented using this)
  /* 95 */ process_subcommand_unimplemented,
  /* 96 */ process_subcommand_unimplemented,
  /* 97 */ process_subcommand_unimplemented,
  /* 98 */ process_subcommand_unimplemented,
  /* 99 */ process_subcommand_unimplemented,
  /* 9A */ process_subcommand_forward_check_size_game, // Update player stat ($infhp/$inftp are implemented using this command)
  /* 9B */ process_subcommand_unimplemented,
  /* 9C */ process_subcommand_forward_check_size_game,
  /* 9D */ process_subcommand_unimplemented,
  /* 9E */ process_subcommand_unimplemented,
  /* 9F */ process_subcommand_forward_check_size_game, // Gal Gryphon actions
  /* A0 */ process_subcommand_forward_check_size_game, // Gal Gryphon actions
  /* A1 */ process_subcommand_unimplemented,
  /* A2 */ process_subcommand_box_drop_item, // Request for item drop from box (handled by server on BB)
  /* A3 */ process_subcommand_forward_check_size_game, // Episode 2 boss actions
  /* A4 */ process_subcommand_forward_check_size_game, // Olga Flow phase 1 actions
  /* A5 */ process_subcommand_forward_check_size_game, // Olga Flow phase 2 actions
  /* A6 */ process_subcommand_forward_check_size, // trade proposal
  /* A7 */ process_subcommand_unimplemented,
  /* A8 */ process_subcommand_forward_check_size_game, // Gol Dragon actions
  /* A9 */ process_subcommand_forward_check_size_game, // Barba Ray actions
  /* AA */ process_subcommand_forward_check_size_game, // Episode 2 boss actions
  /* AB */ process_subcommand_forward_check_size_client, // Create lobby chair
  /* AC */ process_subcommand_unimplemented,
  /* AD */ process_subcommand_forward_check_size_game, // Olga Flow phase 2 subordinate boss actions
  /* AE */ process_subcommand_forward_check_size_client,
  /* AF */ process_subcommand_forward_check_size_client, // Turn in lobby chair
  /* B0 */ process_subcommand_forward_check_size_client, // Move in lobby chair
  /* B1 */ process_subcommand_unimplemented,
  /* B2 */ process_subcommand_unimplemented,
  /* B3 */ process_subcommand_unimplemented,
  /* B4 */ process_subcommand_unimplemented,
  /* B5 */ process_subcommand_open_shop_or_ep3_unknown, // BB shop request
  /* B6 */ process_subcommand_unimplemented, // BB shop contents (server->client only)
  /* B7 */ process_subcommand_unimplemented, // TODO: BB buy shop item
  /* B8 */ process_subcommand_identify_item, // Accept tekker result
  /* B9 */ process_subcommand_unimplemented,
  /* BA */ process_subcommand_unimplemented,
  /* BB */ process_subcommand_open_bank, // BB Bank request
  /* BC */ process_subcommand_unimplemented, // BB bank contents (server->client only)
  /* BD */ process_subcommand_bank_action,
  /* BE */ process_subcommand_unimplemented, // BB create inventory item (server->client only)
  /* BF */ process_subcommand_forward_check_size_ep3_lobby, // Ep3 change music, also BB give EXP (BB usage is server->client only)
  /* C0 */ process_subcommand_unimplemented,
  /* C1 */ process_subcommand_unimplemented,
  /* C2 */ process_subcommand_unimplemented,
  /* C3 */ process_subcommand_drop_stacked_item, // Split stacked item - not sent if entire stack is dropped
  /* C4 */ process_subcommand_sort_inventory,
  /* C5 */ process_subcommand_unimplemented,
  /* C6 */ process_subcommand_unimplemented,
  /* C7 */ process_subcommand_unimplemented,
  /* C8 */ process_subcommand_monster_killed,
  /* C9 */ process_subcommand_unimplemented,
  /* CA */ process_subcommand_unimplemented,
  /* CB */ process_subcommand_unimplemented,
  /* CC */ process_subcommand_unimplemented,
  /* CD */ process_subcommand_unimplemented,
  /* CE */ process_subcommand_unimplemented,
  /* CF */ process_subcommand_forward_check_size_game,
  /* D0 */ process_subcommand_unimplemented,
  /* D1 */ process_subcommand_unimplemented,
  /* D2 */ process_subcommand_unimplemented,
  /* D3 */ process_subcommand_unimplemented,
  /* D4 */ process_subcommand_unimplemented,
  /* D5 */ process_subcommand_unimplemented,
  /* D6 */ process_subcommand_unimplemented,
  /* D7 */ process_subcommand_unimplemented,
  /* D8 */ process_subcommand_unimplemented,
  /* D9 */ process_subcommand_unimplemented,
  /* DA */ process_subcommand_unimplemented,
  /* DB */ process_subcommand_unimplemented,
  /* DC */ process_subcommand_unimplemented,
  /* DD */ process_subcommand_unimplemented,
  /* DE */ process_subcommand_unimplemented,
  /* DF */ process_subcommand_unimplemented,
  /* E0 */ process_subcommand_unimplemented,
  /* E1 */ process_subcommand_unimplemented,
  /* E2 */ process_subcommand_unimplemented,
  /* E3 */ process_subcommand_unimplemented,
  /* E4 */ process_subcommand_unimplemented,
  /* E5 */ process_subcommand_unimplemented,
  /* E6 */ process_subcommand_unimplemented,
  /* E7 */ process_subcommand_unimplemented,
  /* E8 */ process_subcommand_unimplemented,
  /* E9 */ process_subcommand_unimplemented,
  /* EA */ process_subcommand_unimplemented,
  /* EB */ process_subcommand_unimplemented,
  /* EC */ process_subcommand_unimplemented,
  /* ED */ process_subcommand_unimplemented,
  /* EE */ process_subcommand_unimplemented,
  /* EF */ process_subcommand_unimplemented,
  /* F0 */ process_subcommand_unimplemented,
  /* F1 */ process_subcommand_unimplemented,
  /* F2 */ process_subcommand_unimplemented,
  /* F3 */ process_subcommand_unimplemented,
  /* F4 */ process_subcommand_unimplemented,
  /* F5 */ process_subcommand_unimplemented,
  /* F6 */ process_subcommand_unimplemented,
  /* F7 */ process_subcommand_unimplemented,
  /* F8 */ process_subcommand_unimplemented,
  /* F9 */ process_subcommand_unimplemented,
  /* FA */ process_subcommand_unimplemented,
  /* FB */ process_subcommand_unimplemented,
  /* FC */ process_subcommand_unimplemented,
  /* FD */ process_subcommand_unimplemented,
  /* FE */ process_subcommand_unimplemented,
  /* FF */ process_subcommand_unimplemented,
};

void process_subcommand(shared_ptr<ServerState> s, shared_ptr<Lobby> l,
    shared_ptr<Client> c, uint8_t command, uint8_t flag,
    const PSOSubcommand* sub, size_t count) {
  subcommand_handlers[sub->byte[0]](s, l, c, command, flag, sub, count);
}

bool subcommand_is_implemented(uint8_t which) {
  return subcommand_handlers[which] != process_subcommand_unimplemented;
}
