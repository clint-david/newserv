#include "ProxyServer.hh"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/listener.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <iostream>
#include <phosg/Encoding.hh>
#include <phosg/Filesystem.hh>
#include <phosg/Hash.hh>
#include <phosg/Network.hh>
#include <phosg/Random.hh>
#include <phosg/Strings.hh>
#include <phosg/Time.hh>
#ifdef HAVE_RESOURCE_FILE
#include <resource_file/Emulators/PPC32Emulator.hh>
#endif

#include "Compression.hh"
#include "PSOProtocol.hh"
#include "SendCommands.hh"
#include "ReceiveCommands.hh"
#include "ReceiveSubcommands.hh"

using namespace std;



static void forward_command(ProxyServer::LinkedSession& session, bool to_server,
    uint16_t command, uint32_t flag, string& data) {
  auto* bev = to_server ? session.server_bev.get() : session.client_bev.get();
  if (!bev) {
    session.log(WARNING, "No endpoint is present; dropping command");
  } else {
    // Note: we intentionally don't pass name_str here because we already
    // printed the command before calling the handler
    send_command(
        bev,
        session.version,
        to_server ? session.server_output_crypt.get() : session.client_output_crypt.get(),
        command,
        flag,
        data.data(),
        data.size());
  }
}

static void check_implemented_subcommand(uint64_t id, const string& data) {
  if (data.size() < 4) {
    log(WARNING, "[ProxyServer/%08" PRIX64 "] Received broadcast/target command with no contents", id);
  } else {
    if (!subcommand_is_implemented(data[0])) {
      log(WARNING, "[ProxyServer/%08" PRIX64 "] Received subcommand %02hhX which is not implemented on the server",
          id, data[0]);
    }
  }
}



static void send_text_message_to_client(
    ProxyServer::LinkedSession& session,
    uint8_t command,
    const std::string& message) {
  StringWriter w;
  w.put<SC_TextHeader_01_06_11_B0_EE>({0, 0});
  if (session.version == GameVersion::PC) {
    auto decoded = decode_sjis(message);
    w.write(decoded.data(), decoded.size() * sizeof(decoded[0]));
    w.put_u16l(0);
  } else {
    w.write(message);
    w.put_u8(0);
  }
  while (w.size() & 3) {
    w.put_u8(0);
  }
  session.send_to_end(false, command, 0x00, w.str());
}



// Command handlers. These are called to preprocess or react to specific
// commands in either direction. If they return true, the command (which the
// function may have modified) is forwarded to the other end; if they return
// false; it is not.

static bool process_default(shared_ptr<ServerState>,
    ProxyServer::LinkedSession&, uint16_t, uint32_t, string&) {
  return true;
}

static bool process_server_97(shared_ptr<ServerState>,
    ProxyServer::LinkedSession& session, uint16_t, uint32_t, string&) {
  // Trap 97 commands and always send 97 01 04 00. (If flag is 0, the client
  // triggers cheat protection and deletes a bunch of data.)
  session.send_to_end(false, 0x97, 0x01);
  // Also, update the newserv client config so we'll know not to show the
  // programs menu if they return to newserv.
  session.newserv_client_config.cfg.flags |= Client::Flag::SAVE_ENABLED;
  return false;
}

static bool process_server_gc_9A(shared_ptr<ServerState>,
    ProxyServer::LinkedSession& session, uint16_t, uint32_t, string&) {
  if (!session.license) {
    return true;
  }

  C_LoginWithUnusedSpace_GC_9E cmd;
  if (session.remote_guild_card_number == 0) {
    cmd.player_tag = 0xFFFF0000;
    cmd.guild_card_number = 0xFFFFFFFF;
  } else {
    cmd.player_tag = 0x00010000;
    cmd.guild_card_number = session.remote_guild_card_number;
  }
  cmd.unused = 0;
  cmd.sub_version = session.sub_version;
  cmd.unused2.data()[1] = 1;
  cmd.serial_number = string_printf("%08" PRIX32 "", session.license->serial_number);
  cmd.access_key = session.license->access_key;
  cmd.serial_number2 = cmd.serial_number;
  cmd.access_key2 = cmd.access_key;
  cmd.name = session.character_name;
  cmd.client_config.data = session.remote_client_config_data;

  // If there's a guild card number, a shorter 9E is sent that ends
  // right after the client config data

  session.send_to_end(
      true, 0x9E, 0x01, &cmd, 
      sizeof(C_LoginWithUnusedSpace_GC_9E) - (session.remote_guild_card_number ? sizeof(cmd.unused_space) : 0));
  return false;
}

static bool process_server_pc_gc_patch_02_17(shared_ptr<ServerState> s,
    ProxyServer::LinkedSession& session, uint16_t command, uint32_t flag, string& data) {
  if (session.version == GameVersion::PATCH && command == 0x17) {
    throw invalid_argument("patch server sent 17 server init");
  }

  // Most servers don't include after_message or have a shorter
  // after_message than newserv does, so don't require it
  const auto& cmd = check_size_t<S_ServerInit_DC_PC_GC_02_17_92_9B>(data,
      offsetof(S_ServerInit_DC_PC_GC_02_17_92_9B, after_message), 0xFFFF);

  if (!session.license) {
    session.log(INFO, "No license in linked session");

    // We have to forward the command before setting up encryption, so the
    // client will be able to understand it.
    forward_command(session, false, command, flag, data);

    if (session.version == GameVersion::GC) {
      session.server_input_crypt.reset(new PSOGCEncryption(cmd.server_key));
      session.server_output_crypt.reset(new PSOGCEncryption(cmd.client_key));
      session.client_input_crypt.reset(new PSOGCEncryption(cmd.client_key));
      session.client_output_crypt.reset(new PSOGCEncryption(cmd.server_key));
    } else { // PC or patch server (they both use PC encryption)
      session.server_input_crypt.reset(new PSOPCEncryption(cmd.server_key));
      session.server_output_crypt.reset(new PSOPCEncryption(cmd.client_key));
      session.client_input_crypt.reset(new PSOPCEncryption(cmd.client_key));
      session.client_output_crypt.reset(new PSOPCEncryption(cmd.server_key));
    }

    return false;
  }

  session.log(INFO, "Existing license in linked session");

  // This isn't forwarded to the client, so don't recreate the client's crypts
  if ((session.version == GameVersion::PATCH) || (session.version == GameVersion::PC)) {
    session.server_input_crypt.reset(new PSOPCEncryption(cmd.server_key));
    session.server_output_crypt.reset(new PSOPCEncryption(cmd.client_key));
  } else if (session.version == GameVersion::GC) {
    session.server_input_crypt.reset(new PSOGCEncryption(cmd.server_key));
    session.server_output_crypt.reset(new PSOGCEncryption(cmd.client_key));
  } else {
    throw invalid_argument("unsupported version");
  }

  // Respond with an appropriate login command. We don't let the client do this
  // because it believes it already did (when it was in an unlinked session, or
  // in the patch server case, during the current session due to a hidden
  // redirect).
  if (session.version == GameVersion::PATCH) {
    session.send_to_end(true, 0x02, 0x00);
    return false;

  } else if (session.version == GameVersion::PC) {
    C_Login_PC_9D cmd;
    if (session.remote_guild_card_number == 0) {
      cmd.player_tag = 0xFFFF0000;
      cmd.guild_card_number = 0xFFFFFFFF;
    } else {
      cmd.player_tag = 0x00010000;
      cmd.guild_card_number = session.remote_guild_card_number;
    }
    cmd.unused = 0xFFFFFFFFFFFF0000;
    cmd.sub_version = session.sub_version;
    cmd.unused2.data()[1] = 1;
    cmd.serial_number = string_printf("%08" PRIX32 "",
        session.license->serial_number);
    cmd.access_key = session.license->access_key;
    cmd.serial_number2 = cmd.serial_number;
    cmd.access_key2 = cmd.access_key;
    cmd.name = session.character_name;
    session.send_to_end(true, 0x9D, 0x00, &cmd, sizeof(cmd));
    return false;

  } else if (session.version == GameVersion::GC) {
    if (command == 0x17) {
      C_VerifyLicense_GC_DB cmd;
      cmd.serial_number = string_printf("%08" PRIX32 "",
          session.license->serial_number);
      cmd.access_key = session.license->access_key;
      cmd.sub_version = session.sub_version;
      cmd.serial_number2 = cmd.serial_number;
      cmd.access_key2 = cmd.access_key;
      cmd.password = session.license->gc_password;
      session.send_to_end(true, 0xDB, 0x00, &cmd, sizeof(cmd));
      return false;

    } else {
      // For command 02, send the same as if we had received 9A from the server
      return process_server_gc_9A(s, session, command, flag, data);
    }

  } else {
    throw logic_error("invalid game version in server init handler");
  }
}

static bool process_server_bb_03(shared_ptr<ServerState> s,
    ProxyServer::LinkedSession& session, uint16_t, uint32_t, string& data) {
  // Most servers don't include after_message or have a shorter after_message
  // than newserv does, so don't require it
  const auto& cmd = check_size_t<S_ServerInit_BB_03>(data,
      offsetof(S_ServerInit_BB_03, after_message), 0xFFFF);

  // If the session has a detector crypt, then it was resumed from an unlinked
  // session, during which we already sent an 03 command.
  if (session.detector_crypt.get()) {
    if (session.login_command_bb.empty()) {
      throw logic_error("linked BB session does not have a saved login command");
    }

    // This isn't forwarded to the client, so only recreate the server's crypts.
    // Use the same crypt type as the client... the server has the luxury of
    // being able to try all the crypts it knows to detect what type the client
    // uses, but the client can't do this since it sends the first encrypted
    // data on the connection.
    session.server_input_crypt.reset(new PSOBBMultiKeyImitatorEncryption(
        session.detector_crypt, cmd.server_key.data(), sizeof(cmd.server_key), false));
    session.server_output_crypt.reset(new PSOBBMultiKeyImitatorEncryption(
        session.detector_crypt, cmd.client_key.data(), sizeof(cmd.client_key), false));

    // Forward the login command we saved during the unlinked session.
    if (session.enable_remote_ip_crc_patch && (session.login_command_bb.size() >= 0x98)) {
      *reinterpret_cast<le_uint32_t*>(session.login_command_bb.data() + 0x94) =
          session.remote_ip_crc ^ (1309539928UL + 1248334810UL);
    }
    session.send_to_end(true, 0x93, 0x00, session.login_command_bb);

    return false;

  // If there's no detector crypt, then the session is new and was linked
  // immediately at connect time, and an 03 was not yet sent to the client, so
  // we should forward this one.
  } else {
    // Forward the command to the client before setting up the crypts, so the
    // client receives the unencrypted data
    session.send_to_end(false, 0x03, 0x00, data);

    static const string expected_first_data("\xB4\x00\x93\x00\x00\x00\x00\x00", 8);
    session.detector_crypt.reset(new PSOBBMultiKeyDetectorEncryption(
        s->bb_private_keys, expected_first_data, cmd.client_key.data(), sizeof(cmd.client_key)));
    session.client_input_crypt = session.detector_crypt;
    session.client_output_crypt.reset(new PSOBBMultiKeyImitatorEncryption(
        session.detector_crypt, cmd.server_key.data(), sizeof(cmd.server_key), true));
    session.server_input_crypt.reset(new PSOBBMultiKeyImitatorEncryption(
        session.detector_crypt, cmd.server_key.data(), sizeof(cmd.server_key), false));
    session.server_output_crypt.reset(new PSOBBMultiKeyImitatorEncryption(
        session.detector_crypt, cmd.client_key.data(), sizeof(cmd.client_key), false));

    // We already forwarded the command, so don't do so again
    return false;
  }
}

static bool process_server_dc_pc_gc_04(shared_ptr<ServerState>,
    ProxyServer::LinkedSession& session, uint16_t, uint32_t, string& data) {
  // Some servers send a short 04 command if they don't use all of the 0x20
  // bytes available. We should be prepared to handle that.
  auto& cmd = check_size_t<S_UpdateClientConfig_DC_PC_GC_04>(data,
      offsetof(S_UpdateClientConfig_DC_PC_GC_04, cfg),
      sizeof(S_UpdateClientConfig_DC_PC_GC_04));

  // If this is a licensed session, hide the guild card number assigned by the
  // remote server so the client doesn't see it change. If this is an unlicensed
  // session, then the client never received a guild card number from newserv
  // anyway, so we can let the client see the number from the remote server.
  bool had_guild_card_number = (session.remote_guild_card_number != 0);
  if (session.remote_guild_card_number != cmd.guild_card_number) {
    session.remote_guild_card_number = cmd.guild_card_number;
    session.log(INFO, "Remote guild card number set to %" PRIu32,
        session.remote_guild_card_number);
    send_text_message_to_client(session, 0x11, string_printf(
        "The remote server\nhas assigned your\nGuild Card number as\n\tC6%" PRIu32,
        session.remote_guild_card_number));
  }
  if (session.license) {
    cmd.guild_card_number = session.license->serial_number;
  }

  // It seems the client ignores the length of the 04 command, and always copies
  // 0x20 bytes to its config data. So if the server sends a short 04 command,
  // part of the previous command ends up in the security data (usually part of
  // the copyright string from the server init command). We simulate that here.
  // If there was previously a guild card number, assume we got the lobby server
  // init text instead of the port map init text.
  memcpy(session.remote_client_config_data.data(),
      had_guild_card_number
        ? "t Lobby Server. Copyright SEGA E"
        : "t Port Map. Copyright SEGA Enter",
      session.remote_client_config_data.bytes());
  memcpy(session.remote_client_config_data.data(), &cmd.cfg,
      min<size_t>(data.size() - sizeof(S_UpdateClientConfig_DC_PC_GC_04),
        session.remote_client_config_data.bytes()));

  // If the guild card number was not set, pretend (to the server) that this is
  // the first 04 command the client has received. The client responds with a 96
  // (checksum) in that case.
  if (!had_guild_card_number) {
    // We don't actually have a client checksum, of course... hopefully just
    // random data will do (probably no private servers check this at all)
    le_uint64_t checksum = random_object<uint64_t>() & 0x0000FFFFFFFFFFFF;
    session.send_to_end(true, 0x96, 0x00, &checksum, sizeof(checksum));
  }

  return true;
}

static bool process_server_dc_pc_gc_06(shared_ptr<ServerState>,
    ProxyServer::LinkedSession& session, uint16_t, uint32_t, string& data) {
  if (session.license) {
    auto& cmd = check_size_t<SC_TextHeader_01_06_11_B0_EE>(data,
        sizeof(SC_TextHeader_01_06_11_B0_EE), 0xFFFF);
    if (cmd.guild_card_number == session.remote_guild_card_number) {
      cmd.guild_card_number = session.license->serial_number;
    }
  }
  return true;
}

template <typename CmdT>
static bool process_server_41(shared_ptr<ServerState>,
    ProxyServer::LinkedSession& session, uint16_t, uint32_t, string& data) {
  if (session.license) {
    auto& cmd = check_size_t<CmdT>(data);
    if (cmd.searcher_guild_card_number == session.remote_guild_card_number) {
      cmd.searcher_guild_card_number = session.license->serial_number;
    }
    if (cmd.result_guild_card_number == session.remote_guild_card_number) {
      cmd.result_guild_card_number = session.license->serial_number;
    }
  }
  return true;
}

template <typename CmdT>
static bool process_server_81(shared_ptr<ServerState>,
    ProxyServer::LinkedSession& session, uint16_t, uint32_t, string& data) {
  if (session.license) {
    auto& cmd = check_size_t<CmdT>(data);
    if (cmd.from_guild_card_number == session.remote_guild_card_number) {
      cmd.from_guild_card_number = session.license->serial_number;
    }
    if (cmd.to_guild_card_number == session.remote_guild_card_number) {
      cmd.to_guild_card_number = session.license->serial_number;
    }
  }
  return true;
}

static bool process_server_88(shared_ptr<ServerState>,
    ProxyServer::LinkedSession& session, uint16_t, uint32_t flag, string& data) {
  if (session.license) {
    size_t expected_size = sizeof(S_ArrowUpdateEntry_88) * flag;
    auto* entries = &check_size_t<S_ArrowUpdateEntry_88>(data,
        expected_size, expected_size);
    for (size_t x = 0; x < flag; x++) {
      if (entries[x].guild_card_number == session.remote_guild_card_number) {
        entries[x].guild_card_number = session.license->serial_number;
      }
    }
  }
  return true;
}

static bool process_server_B2(shared_ptr<ServerState>,
    ProxyServer::LinkedSession& session, uint16_t, uint32_t flag, string& data) {
  if (session.save_files) {
    string output_filename = string_printf("code.%" PRId64 ".bin", now());
    save_file(output_filename, data);
    session.log(INFO, "Wrote code from server to file %s", output_filename.c_str());

#ifdef HAVE_RESOURCE_FILE
    try {
      // Note: we copy header here because we might modify data later, which
      // would break the reference
      auto header = StringReader(data).get<S_ExecuteCode_B2>();

      size_t footer_end_offset = header.code_size;
      size_t footer_offset = footer_end_offset - sizeof(S_ExecuteCode_Footer_GC_B2);
      size_t orig_size = data.size() - sizeof(header);
      if (data.size() < (sizeof(header) + footer_end_offset)) {
        data.resize((sizeof(header) + footer_end_offset), '\0');
      }

      fprintf(stderr, "footer_offset = %08zX\n", footer_offset);
      print_data(stderr, data);

      StringReader r(data.data() + sizeof(header), data.size() - sizeof(header));
      const auto& footer = r.pget<S_ExecuteCode_Footer_GC_B2>(footer_offset);

      multimap<uint32_t, string> labels;
      r.go(footer.relocations_offset);
      uint32_t reloc_offset = 0;
      for (size_t x = 0; x < footer.num_relocations; x++) {
        reloc_offset += (r.get_u16b() * 4);
        labels.emplace(reloc_offset, string_printf("reloc%zu", x));
      }
      labels.emplace(footer.entrypoint_addr_offset.load(), "entry_ptr");
      labels.emplace(footer_offset, "footer");
      labels.emplace(r.pget_u32b(footer.entrypoint_addr_offset), "start");

      for (const auto& it : labels) {
        fprintf(stderr, "label: %08" PRIX32 " => %s\n", it.first, it.second.c_str());
      }

      string disassembly = PPC32Emulator::disassemble(
          &r.pget<uint8_t>(0, orig_size),
          orig_size,
          0,
          &labels);

      output_filename = string_printf("code.%" PRId64 ".txt", now());
      {
        auto f = fopen_unique(output_filename, "wt");
        fprintf(f.get(), "// code_size = 0x%" PRIX32 "\n", header.code_size.load());
        fprintf(f.get(), "// checksum_addr = 0x%" PRIX32 "\n", header.checksum_start.load());
        fprintf(f.get(), "// checksum_size = 0x%" PRIX32 "\n", header.checksum_size.load());
        fwritex(f.get(), disassembly);
      }
      session.log(INFO, "Wrote disassembly to file %s", output_filename.c_str());

    } catch (const exception& e) {
      session.log(INFO, "Failed to disassemble code from server: %s", e.what());
    }
#endif
  }

  if (session.function_call_return_value >= 0) {
    session.log(INFO, "Blocking function call from server");
    C_ExecuteCodeResult_B3 cmd;
    cmd.return_value = session.function_call_return_value;
    cmd.checksum = 0;
    session.send_to_end(true, 0xB3, flag, &cmd, sizeof(cmd));
    return false;
  } else {
    return true;
  }
}

static bool process_server_E7(shared_ptr<ServerState>,
    ProxyServer::LinkedSession& session, uint16_t, uint32_t, string& data) {
  if (session.save_files) {
    string output_filename = string_printf("player.%" PRId64 ".bin", now());
    save_file(output_filename, data);
    session.log(INFO, "Wrote player data to file %s", output_filename.c_str());
  }
  return true;
}

template <typename CmdT>
static bool process_server_C4(shared_ptr<ServerState>,
    ProxyServer::LinkedSession& session, uint16_t, uint32_t flag, string& data) {
  if (session.license) {
    size_t expected_size = sizeof(CmdT) * flag;
    // Some servers (e.g. Schtserv) send extra data on the end of this command;
    // the client ignores it so we can ignore it too
    auto* entries = &check_size_t<CmdT>(data, expected_size, 0xFFFF);
    for (size_t x = 0; x < flag; x++) {
      if (entries[x].guild_card_number == session.remote_guild_card_number) {
        entries[x].guild_card_number = session.license->serial_number;
      }
    }
  }
  return true;
}

static bool process_server_gc_E4(shared_ptr<ServerState>,
    ProxyServer::LinkedSession& session, uint16_t, uint32_t, string& data) {
  auto& cmd = check_size_t<S_CardLobbyGame_GC_E4>(data);
  for (size_t x = 0; x < 4; x++) {
    if (cmd.entries[x].guild_card_number == session.remote_guild_card_number) {
      cmd.entries[x].guild_card_number = session.license->serial_number;
    }
  }
  return true;
}

static bool process_server_bb_22(shared_ptr<ServerState>,
    ProxyServer::LinkedSession& session, uint16_t, uint32_t, string& data) {
  // We use this command (which is sent before the init encryption command) to
  // detect a particular server behavior that we'll have to work around later.
  // It looks like this command's existence is another anti-proxy measure, since
  // this command is 0x34 bytes in total, and the logic that adds padding bytes
  // when the command size isn't a multiple of 8 is only active when encryption
  // is enabled. Presumably some simpler proxies would get this wrong.
  // Editor's note: There's an unsavory message in this command's data field,
  // hence the hash here instead of a direct string comparison. I'd love to hear
  // the story behind why they put that string there.
  if ((data.size() == 0x2C) &&
      (fnv1a64(data.data(), data.size()) == 0x8AF8314316A27994)) {
    session.log(INFO, "Enabling remote IP CRC patch");
    session.enable_remote_ip_crc_patch = true;
  }
  return true;
}

static bool process_server_game_19_patch_14(shared_ptr<ServerState>,
    ProxyServer::LinkedSession& session, uint16_t command, uint32_t, string& data) {
  // If the command is shorter than 6 bytes, use the previous server command to
  // fill it in. This simulates a behavior used by some private servers where a
  // longer previous command is used to fill part of the client's receive buffer
  // with meaningful data, then an intentionally undersize 19 command is sent
  // which results in the client using the previous command's data as part of
  // the 19 command's contents. They presumably do this in an attempt to prevent
  // people from using proxies.
  if (data.size() < sizeof(session.prev_server_command_bytes)) {
    data.append(
        reinterpret_cast<const char*>(&session.prev_server_command_bytes[data.size()]),
        sizeof(session.prev_server_command_bytes) - data.size());
  }
  if (data.size() < sizeof(S_Reconnect_19)) {
    data.resize(sizeof(S_Reconnect_19), '\0');
  }

  if (session.enable_remote_ip_crc_patch) {
    session.remote_ip_crc = crc32(data.data(), 4);
  }

  // This weird maximum size is here to properly handle the version-split
  // command that some servers (including newserv) use on port 9100
  auto& cmd = check_size_t<S_Reconnect_19>(data, sizeof(S_Reconnect_19), 0xB0);
  memset(&session.next_destination, 0, sizeof(session.next_destination));
  struct sockaddr_in* sin = reinterpret_cast<struct sockaddr_in*>(
      &session.next_destination);
  sin->sin_family = AF_INET;
  sin->sin_addr.s_addr = cmd.address.load_raw();
  sin->sin_port = htons(cmd.port);

  if (!session.client_bev.get()) {
    session.log(WARNING, "Received reconnect command with no destination present");
    return false;

  } else if (command == 0x14) {
    // On the patch server, hide redirects from the client completely. The new
    // destination server will presumably send a new 02 command to start
    // encryption; it appears that PSOBB doesn't fail if this happens, and
    // simply re-initializes its encryption appropriately.
    session.server_input_crypt.reset();
    session.server_output_crypt.reset();

    struct sockaddr_in* dest_sin = reinterpret_cast<sockaddr_in*>(
        &session.next_destination);
    dest_sin->sin_family = AF_INET;
    dest_sin->sin_addr.s_addr = cmd.address.load_raw();
    dest_sin->sin_port = cmd.port;
    session.connect();
    return false;

  } else {
    // If the client is on a virtual connection (fd < 0), only change
    // the port (so we'll know which version to treat the next
    // connection as). It's better to leave the address as-is so we
    // can circumvent the Plus/Ep3 same-network-server check.
    int fd = bufferevent_getfd(session.client_bev.get());
    if (fd >= 0) {
      struct sockaddr_storage sockname_ss;
      socklen_t len = sizeof(sockname_ss);
      getsockname(fd, reinterpret_cast<struct sockaddr*>(&sockname_ss), &len);
      if (sockname_ss.ss_family != AF_INET) {
        throw logic_error("existing connection is not ipv4");
      }

      struct sockaddr_in* sockname_sin = reinterpret_cast<struct sockaddr_in*>(
          &sockname_ss);
      cmd.address.store_raw(sockname_sin->sin_addr.s_addr);
      cmd.port = ntohs(sockname_sin->sin_port);

    } else {
      cmd.port = session.local_port;
    }
    return true;
  }
}

static bool process_server_gc_1A_D5(shared_ptr<ServerState>,
    ProxyServer::LinkedSession& session, uint16_t, uint32_t, string&) {
  // If the client has the no-close-confirmation flag set in its
  // newserv client config, send a fake confirmation to the remote
  // server immediately.
  if (session.newserv_client_config.cfg.flags & Client::Flag::NO_MESSAGE_BOX_CLOSE_CONFIRMATION) {
    session.send_to_end(true, 0xD6, 0x00, "", 0);
  }
  return true;
}

static bool process_server_60_62_6C_6D_C9_CB(shared_ptr<ServerState>,
    ProxyServer::LinkedSession& session, uint16_t, uint32_t, string& data) {
  check_implemented_subcommand(session.id, data);

  if (session.save_files) {
    if ((session.version == GameVersion::GC) && (data.size() >= 0x14)) {
      PSOSubcommand* subs = &check_size_t<PSOSubcommand>(data, 0x14, 0xFFFF);
      if (subs[0].dword == 0x000000B6 && subs[2].dword == 0x00000041) {
        string filename = string_printf("map%08" PRIX32 ".%" PRIu64 ".mnmd",
            subs[3].dword.load(), now());
        string map_data = prs_decompress(data.substr(0x14));
        save_file(filename, map_data);
        session.log(INFO, "Wrote %zu bytes to %s", map_data.size(), filename.c_str());
      }
    }
  }

  return true;
}

template <typename T>
static bool process_server_44_A6(shared_ptr<ServerState>,
    ProxyServer::LinkedSession& session, uint16_t command, uint32_t, string& data) {
  if (session.save_files) {
    const auto& cmd = check_size_t<S_OpenFile_PC_GC_44_A6>(data);
    bool is_download_quest = (command == 0xA6);

    string filename = cmd.filename;
    string output_filename = string_printf("%s.%s.%" PRIu64,
        filename.c_str(),
        is_download_quest ? "download" : "online", now());
    for (size_t x = 0; x < output_filename.size(); x++) {
      if (output_filename[x] < 0x20 || output_filename[x] > 0x7E || output_filename[x] == '/') {
        output_filename[x] = '_';
      }
    }
    if (output_filename[0] == '.') {
      output_filename[0] = '_';
    }

    ProxyServer::LinkedSession::SavingFile sf(
        cmd.filename, output_filename, cmd.file_size);
    session.saving_files.emplace(cmd.filename, move(sf));
    session.log(INFO, "Opened file %s", output_filename.c_str());
  }
  return true;
}

static bool process_server_13_A7(shared_ptr<ServerState>,
    ProxyServer::LinkedSession& session, uint16_t, uint32_t, string& data) {
  if (session.save_files) {
    const auto& cmd = check_size_t<S_WriteFile_13_A7>(data);

    ProxyServer::LinkedSession::SavingFile* sf = nullptr;
    try {
      sf = &session.saving_files.at(cmd.filename);
    } catch (const out_of_range&) {
      string filename = cmd.filename;
      session.log(WARNING, "Received data for non-open file %s", filename.c_str());
      return true;
    }

    size_t bytes_to_write = cmd.data_size;
    if (bytes_to_write > 0x400) {
      session.log(WARNING, "Chunk data size is invalid; truncating to 0x400");
      bytes_to_write = 0x400;
    }

    session.log(INFO, "Writing %zu bytes to %s", bytes_to_write, sf->output_filename.c_str());
    fwritex(sf->f.get(), cmd.data, bytes_to_write);
    if (bytes_to_write > sf->remaining_bytes) {
      session.log(WARNING, "Chunk size extends beyond original file size; file may be truncated");
      sf->remaining_bytes = 0;
    } else {
      sf->remaining_bytes -= bytes_to_write;
    }

    if (sf->remaining_bytes == 0) {
      session.log(INFO, "File %s is complete", sf->output_filename.c_str());
      session.saving_files.erase(cmd.filename);
    }
  }
  return true;
}

static bool process_server_gc_B8(shared_ptr<ServerState>,
    ProxyServer::LinkedSession& session, uint16_t, uint32_t, string& data) {
  if (session.save_files) {
    if (data.size() < 4) {
      session.log(WARNING, "Card list data size is too small; skipping file");
      return true;
    }

    StringReader r(data);
    size_t size = r.get_u32l();
    if (r.remaining() < size) {
      session.log(WARNING, "Card list data size extends beyond end of command; skipping file");
      return true;
    }

    string output_filename = string_printf("cardupdate.%" PRIu64 ".mnr", now());
    save_file(output_filename, r.read(size));
    session.log(INFO, "Wrote %zu bytes to %s", size, output_filename.c_str());
  }
  return true;
}

template <typename CmdT>
static bool process_server_65_67_68(shared_ptr<ServerState>,
    ProxyServer::LinkedSession& session, uint16_t command, uint32_t flag, string& data) {
  if (command == 0x67) {
    session.lobby_players.clear();
    session.lobby_players.resize(12);
    session.log(INFO, "Cleared lobby players");

    // This command can cause the client to no longer send D6 responses when
    // 1A/D5 large message boxes are closed. newserv keeps track of this
    // behavior in the client config, so if it happens during a proxy session,
    // update the client config that we'll restore if the client uses the change
    // ship or change block command.
    if (session.newserv_client_config.cfg.flags & Client::Flag::NO_MESSAGE_BOX_CLOSE_CONFIRMATION_AFTER_LOBBY_JOIN) {
      session.newserv_client_config.cfg.flags |= Client::Flag::NO_MESSAGE_BOX_CLOSE_CONFIRMATION;
    }
  }

  size_t expected_size = offsetof(CmdT, entries) + sizeof(typename CmdT::Entry) * flag;
  auto& cmd = check_size_t<CmdT>(data, expected_size, expected_size);

  session.lobby_client_id = cmd.client_id;
  for (size_t x = 0; x < flag; x++) {
    size_t index = cmd.entries[x].lobby_data.client_id;
    if (index >= session.lobby_players.size()) {
      session.log(WARNING, "Ignoring invalid player index %zu at position %zu", index, x);
    } else {
      if (session.license && (cmd.entries[x].lobby_data.guild_card == session.remote_guild_card_number)) {
        cmd.entries[x].lobby_data.guild_card = session.license->serial_number;
      }
      session.lobby_players[index].guild_card_number = cmd.entries[x].lobby_data.guild_card;
      ptext<char, 0x10> name = cmd.entries[x].disp.name;
      session.lobby_players[index].name = name;
      session.log(INFO, "Added lobby player: (%zu) %" PRIu32 " %s",
          index,
          session.lobby_players[index].guild_card_number,
          session.lobby_players[index].name.c_str());
    }
  }

  if (session.override_lobby_event >= 0) {
    cmd.event = session.override_lobby_event;
  }
  if (session.override_lobby_number >= 0) {
    cmd.lobby_number = session.override_lobby_number;
  }

  return true;
}

template <typename CmdT>
static bool process_server_64(shared_ptr<ServerState>,
    ProxyServer::LinkedSession& session, uint16_t, uint32_t flag, string& data) {
  // We don't need to clear lobby_players here because we always
  // overwrite all 4 entries for this command
  session.lobby_players.resize(4);
  session.log(INFO, "Cleared lobby players");

  const size_t expected_size = session.sub_version >= 0x40
      ? sizeof(CmdT)
      : offsetof(CmdT, players_ep3);
  auto& cmd = check_size_t<CmdT>(data, expected_size, expected_size);

  session.lobby_client_id = cmd.client_id;
  for (size_t x = 0; x < flag; x++) {
    if (cmd.lobby_data[x].guild_card == session.remote_guild_card_number) {
      cmd.lobby_data[x].guild_card = session.license->serial_number;
    }
    session.lobby_players[x].guild_card_number = cmd.lobby_data[x].guild_card;
    if (data.size() == sizeof(CmdT)) {
      ptext<char, 0x10> name = cmd.players_ep3[x].disp.name;
      session.lobby_players[x].name = name;
    } else {
      session.lobby_players[x].name.clear();
    }
    session.log(INFO, "Added lobby player: (%zu) %" PRIu32 " %s",
        x,
        session.lobby_players[x].guild_card_number,
        session.lobby_players[x].name.c_str());
  }

  if (session.override_section_id >= 0) {
    cmd.section_id = session.override_section_id;
  }
  if (session.override_lobby_event >= 0) {
    cmd.event = session.override_lobby_event;
  }

  return true;
}

static bool process_server_66_69(shared_ptr<ServerState>,
    ProxyServer::LinkedSession& session, uint16_t, uint32_t, string& data) {
  const auto& cmd = check_size_t<S_LeaveLobby_66_69_Ep3_E9>(data);
  size_t index = cmd.client_id;
  if (index >= session.lobby_players.size()) {
    session.log(WARNING, "Lobby leave command references missing position");
  } else {
    session.lobby_players[index].guild_card_number = 0;
    session.lobby_players[index].name.clear();
    session.log(INFO, "Removed lobby player (%zu)", index);
  }
  return true;
}





static bool process_client_06(shared_ptr<ServerState>,
    ProxyServer::LinkedSession& session, uint16_t, uint32_t, string& data) {
  if (data.size() >= 12) {
    // If this chat message looks like a newserv chat command, suppress it
    if (session.suppress_newserv_commands &&
        (data[8] == '$' || (data[8] == '\t' && data[9] != 'C' && data[10] == '$'))) {
      session.log(WARNING, "Chat message appears to be a server command; dropping it");
      return false;
    } else if (session.enable_chat_filter) {
      add_color_inplace(data.data() + 8, data.size() - 8);
    }
  }
  return true;
}

static bool process_client_40(shared_ptr<ServerState>,
    ProxyServer::LinkedSession& session, uint16_t, uint32_t, string& data) {
  if (session.license) {
    auto& cmd = check_size_t<C_GuildCardSearch_40>(data);
    if (cmd.searcher_guild_card_number == session.license->serial_number) {
      cmd.searcher_guild_card_number = session.remote_guild_card_number;
    }
    if (cmd.target_guild_card_number == session.license->serial_number) {
      cmd.target_guild_card_number = session.remote_guild_card_number;
    }
  }
  return true;
}

template <typename CmdT>
static bool process_client_81(shared_ptr<ServerState>,
    ProxyServer::LinkedSession& session, uint16_t, uint32_t, string& data) {
  auto& cmd = check_size_t<SC_SimpleMail_GC_81>(data);
  if (session.license) {
    if (cmd.from_guild_card_number == session.license->serial_number) {
      cmd.from_guild_card_number = session.remote_guild_card_number;
    }
    if (cmd.to_guild_card_number == session.license->serial_number) {
      cmd.to_guild_card_number = session.remote_guild_card_number;
    }
  }
  // GC clients send uninitialized memory here; don't forward it
  cmd.text.clear_after(cmd.text.len());
  return true;
}

template <typename SendGuildCardCmdT>
static bool process_client_60_62_6C_6D_C9_CB(shared_ptr<ServerState> s,
    ProxyServer::LinkedSession& session, uint16_t command, uint32_t flag, string& data) {
  if (session.license && !data.empty()) {
    if (data[0] == 0x06) {
      auto& cmd = check_size_t<SendGuildCardCmdT>(data);
      if (cmd.guild_card_number == session.license->serial_number) {
        cmd.guild_card_number = session.remote_guild_card_number;
      }
    } else if (data[0] == 0x2F || data[0] == 0x4C) {
      if (session.infinite_hp) {
        vector<PSOSubcommand> subs;
        for (size_t amount = 1020; amount > 0;) {
          auto& sub1 = subs.emplace_back();
          sub1.word[0] = 0x029A;
          sub1.byte[2] = session.lobby_client_id;
          sub1.byte[3] = 0x00;
          auto& sub2 = subs.emplace_back();
          sub2.word[0] = 0x0000;
          sub2.byte[2] = PlayerStatsChange::ADD_HP;
          sub2.byte[3] = (amount > 0xFF) ? 0xFF : amount;
          amount -= sub2.byte[3];
        }
        session.send_to_end(false, 0x60, 0x00, subs.data(), subs.size() * sizeof(PSOSubcommand));
      }
    } else if (data[0] == 0x48) {
      if (session.infinite_tp) {
        PSOSubcommand subs[2];
        subs[0].word[0] = 0x029A;
        subs[0].byte[2] = session.lobby_client_id;
        subs[0].byte[3] = 0x00;
        subs[1].word[0] = 0x0000;
        subs[1].byte[2] = PlayerStatsChange::ADD_TP;
        subs[1].byte[3] = 0xFF;
        session.send_to_end(false, 0x60, 0x00, &subs[0], sizeof(subs));
      }
    }
  }
  return process_client_60_62_6C_6D_C9_CB<void>(s, session, command, flag, data);
}

template <>
bool process_client_60_62_6C_6D_C9_CB<void>(shared_ptr<ServerState>,
    ProxyServer::LinkedSession& session, uint16_t, uint32_t, string& data) {
  check_implemented_subcommand(session.id, data);

  if (!data.empty() && (data[0] == 0x05) && session.enable_switch_assist) {
    auto& cmd = check_size_t<G_SwitchStateChanged_6x05>(data);
    if (cmd.enabled && cmd.switch_id != 0xFFFF) {
      if (session.last_switch_enabled_command.subcommand == 0x05) {
        session.log(INFO, "Switch assist: replaying previous enable command");
        session.send_to_end(true, 0x60, 0x00, &session.last_switch_enabled_command,
            sizeof(session.last_switch_enabled_command));
        session.send_to_end(false, 0x60, 0x00, &session.last_switch_enabled_command,
            sizeof(session.last_switch_enabled_command));
      }
      session.last_switch_enabled_command = cmd;
    }
  }

  return true;
}

static bool process_client_dc_pc_gc_A0_A1(shared_ptr<ServerState> s,
    ProxyServer::LinkedSession& session, uint16_t, uint32_t, string&) {
  if (!session.license) {
    return true;
  }

  // For licensed sessions, send them back to newserv's main menu instead of
  // going to the remote server's ship/block select menu

  // Delete all the other players
  for (size_t x = 0; x < session.lobby_players.size(); x++) {
    if (session.lobby_players[x].guild_card_number == 0) {
      continue;
    }
    uint8_t leaving_id = x;
    uint8_t leader_id = session.lobby_client_id;
    S_LeaveLobby_66_69_Ep3_E9 cmd = {leaving_id, leader_id, 0};
    session.send_to_end(false, 0x69, leaving_id, &cmd, sizeof(cmd));
  }

  string encoded_name = encode_sjis(s->name);
  send_text_message_to_client(session, 0x11, string_printf(
      "You\'ve returned to\n\tC6%s", encoded_name.c_str()));

  // Restore newserv_client_config, so the login server gets the client flags
  S_UpdateClientConfig_DC_PC_GC_04 update_client_config_cmd;
  update_client_config_cmd.player_tag = 0x00010000;
  update_client_config_cmd.guild_card_number = session.license->serial_number;
  update_client_config_cmd.cfg = session.newserv_client_config.cfg;
  session.send_to_end(false, 0x04, 0x00, &update_client_config_cmd, sizeof(update_client_config_cmd));

  static const vector<string> version_to_port_name({
      "dc-login", "pc-login", "bb-patch", "gc-us3", "bb-login"});
  const auto& port_name = version_to_port_name.at(static_cast<size_t>(
      session.version));

  S_Reconnect_19 reconnect_cmd = {
      0, s->name_to_port_config.at(port_name)->port, 0};

  // If the client is on a virtual connection, we can use any address
  // here and they should be able to connect back to the game server. If
  // the client is on a real connection, we'll use the sockname of the
  // existing connection (like we do in the server 19 command handler).
  int fd = bufferevent_getfd(session.client_bev.get());
  if (fd < 0) {
    struct sockaddr_in* dest_sin = reinterpret_cast<struct sockaddr_in*>(&session.next_destination);
    if (dest_sin->sin_family != AF_INET) {
      throw logic_error("ss not AF_INET");
    }
    reconnect_cmd.address.store_raw(dest_sin->sin_addr.s_addr);
  } else {
    struct sockaddr_storage sockname_ss;
    socklen_t len = sizeof(sockname_ss);
    getsockname(fd, reinterpret_cast<struct sockaddr*>(&sockname_ss), &len);
    if (sockname_ss.ss_family != AF_INET) {
      throw logic_error("existing connection is not ipv4");
    }

    struct sockaddr_in* sockname_sin = reinterpret_cast<struct sockaddr_in*>(
        &sockname_ss);
    reconnect_cmd.address.store_raw(sockname_sin->sin_addr.s_addr);
  }

  session.send_to_end(false, 0x19, 0x00, &reconnect_cmd, sizeof(reconnect_cmd));

  return false;
}



typedef bool (*process_command_t)(
    shared_ptr<ServerState> s,
    ProxyServer::LinkedSession& session,
    uint16_t command,
    uint32_t flag,
    string& data);

// The entries in these arrays correspond to the ID of the command received. For
// instance, if a command 6C is received, the function at position 0x6C in the
// array corresponding to the client's version is called.
auto defh = process_default;

static process_command_t dc_server_handlers[0x100] = {
  /* 00 */ defh, defh, defh, defh, process_server_dc_pc_gc_04, defh, process_server_dc_pc_gc_06, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 10 */ defh, defh, defh, process_server_13_A7, defh, defh, defh, defh, defh, process_server_game_19_patch_14, defh, defh, defh, defh, defh, defh,
  /* 20 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 30 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 40 */ defh, process_server_41<S_GuildCardSearchResult_DC_GC_41>, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 50 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 60 */ process_server_60_62_6C_6D_C9_CB, defh, process_server_60_62_6C_6D_C9_CB, defh, defh, defh, process_server_66_69, defh, defh, process_server_66_69, defh, defh, process_server_60_62_6C_6D_C9_CB, process_server_60_62_6C_6D_C9_CB, defh, defh,
  /* 70 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 80 */ defh, defh, defh, defh, defh, defh, defh, defh, process_server_88, defh, defh, defh, defh, defh, defh, defh,
  /* 90 */ defh, defh, defh, defh, defh, defh, defh, process_server_97, defh, defh, defh, defh, defh, defh, defh, defh,
  /* A0 */ defh, defh, defh, defh, defh, defh, defh, process_server_13_A7, defh, defh, defh, defh, defh, defh, defh, defh,
  /* B0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* C0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* D0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* E0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* F0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
};
static process_command_t pc_server_handlers[0x100] = {
  /* 00 */ defh, defh, process_server_pc_gc_patch_02_17, defh, process_server_dc_pc_gc_04, defh, process_server_dc_pc_gc_06, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 10 */ defh, defh, defh, process_server_13_A7, defh, defh, defh, process_server_pc_gc_patch_02_17, defh, process_server_game_19_patch_14, defh, defh, defh, defh, defh, defh,
  /* 20 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 30 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 40 */ defh, process_server_41<S_GuildCardSearchResult_PC_41>, defh, defh, process_server_44_A6<S_OpenFile_PC_GC_44_A6>, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 50 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 60 */ process_server_60_62_6C_6D_C9_CB, defh, process_server_60_62_6C_6D_C9_CB, defh, process_server_64<S_JoinGame_PC_64>, process_server_65_67_68<S_JoinLobby_PC_65_67_68>, process_server_66_69, process_server_65_67_68<S_JoinLobby_PC_65_67_68>, process_server_65_67_68<S_JoinLobby_PC_65_67_68>, process_server_66_69, defh, defh, process_server_60_62_6C_6D_C9_CB, process_server_60_62_6C_6D_C9_CB, defh, defh,
  /* 70 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 80 */ defh, defh, defh, defh, defh, defh, defh, defh, process_server_88, defh, defh, defh, defh, defh, defh, defh,
  /* 90 */ defh, defh, defh, defh, defh, defh, defh, process_server_97, defh, defh, defh, defh, defh, defh, defh, defh,
  /* A0 */ defh, defh, defh, defh, defh, defh, process_server_44_A6<S_OpenFile_PC_GC_44_A6>, process_server_13_A7, defh, defh, defh, defh, defh, defh, defh, defh,
  /* B0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* C0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* D0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* E0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* F0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
};
static process_command_t gc_server_handlers[0x100] = {
  /* 00 */ defh, defh, process_server_pc_gc_patch_02_17, defh, process_server_dc_pc_gc_04, defh, process_server_dc_pc_gc_06, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 10 */ defh, defh, defh, process_server_13_A7, defh, defh, defh, process_server_pc_gc_patch_02_17, defh, process_server_game_19_patch_14, process_server_gc_1A_D5, defh, defh, defh, defh, defh,
  /* 20 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 30 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 40 */ defh, process_server_41<S_GuildCardSearchResult_DC_GC_41>, defh, defh, process_server_44_A6<S_OpenFile_PC_GC_44_A6>, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 50 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 60 */ process_server_60_62_6C_6D_C9_CB, defh, process_server_60_62_6C_6D_C9_CB, defh, process_server_64<S_JoinGame_GC_64>, process_server_65_67_68<S_JoinLobby_GC_65_67_68>, process_server_66_69, process_server_65_67_68<S_JoinLobby_GC_65_67_68>, process_server_65_67_68<S_JoinLobby_GC_65_67_68>, process_server_66_69, defh, defh, process_server_60_62_6C_6D_C9_CB, process_server_60_62_6C_6D_C9_CB, defh, defh,
  /* 70 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 80 */ defh, process_server_81<SC_SimpleMail_GC_81>, defh, defh, defh, defh, defh, defh, process_server_88, defh, defh, defh, defh, defh, defh, defh,
  /* 90 */ defh, defh, defh, defh, defh, defh, defh, process_server_97, defh, defh, process_server_gc_9A, defh, defh, defh, defh, defh,
  /* A0 */ defh, defh, defh, defh, defh, defh, process_server_44_A6<S_OpenFile_PC_GC_44_A6>, process_server_13_A7, defh, defh, defh, defh, defh, defh, defh, defh,
  /* B0 */ defh, defh, process_server_B2, defh, defh, defh, defh, defh, process_server_gc_B8, defh, defh, defh, defh, defh, defh, defh,
  /* C0 */ defh, defh, defh, defh, process_server_C4<S_ChoiceSearchResultEntry_GC_C4>, defh, defh, defh, defh, process_server_60_62_6C_6D_C9_CB, defh, process_server_60_62_6C_6D_C9_CB, defh, defh, defh, defh,
  /* D0 */ defh, defh, defh, defh, defh, process_server_gc_1A_D5, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* E0 */ defh, defh, defh, defh, process_server_gc_E4, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* F0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
};
static process_command_t bb_server_handlers[0x100] = {
  /* 00 */ defh, defh, defh, process_server_bb_03, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 10 */ defh, defh, defh, process_server_13_A7, defh, defh, defh, defh, defh, process_server_game_19_patch_14, defh, defh, defh, defh, defh, defh,
  /* 20 */ defh, defh, process_server_bb_22, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 30 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 40 */ defh, process_server_41<S_GuildCardSearchResult_BB_41>, defh, defh, process_server_44_A6<S_OpenFile_BB_44_A6>, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 50 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 60 */ process_server_60_62_6C_6D_C9_CB, defh, process_server_60_62_6C_6D_C9_CB, defh, process_server_64<S_JoinGame_BB_64>, process_server_65_67_68<S_JoinLobby_BB_65_67_68>, process_server_66_69, process_server_65_67_68<S_JoinLobby_BB_65_67_68>, process_server_65_67_68<S_JoinLobby_BB_65_67_68>, process_server_66_69, defh, defh, process_server_60_62_6C_6D_C9_CB, process_server_60_62_6C_6D_C9_CB, defh, defh,
  /* 70 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 80 */ defh, defh, defh, defh, defh, defh, defh, defh, process_server_88, defh, defh, defh, defh, defh, defh, defh,
  /* 90 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* A0 */ defh, defh, defh, defh, defh, defh, process_server_44_A6<S_OpenFile_BB_44_A6>, process_server_13_A7, defh, defh, defh, defh, defh, defh, defh, defh,
  /* B0 */ defh, defh, process_server_B2, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* C0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* D0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* E0 */ defh, defh, defh, defh, defh, defh, defh, process_server_E7, defh, defh, defh, defh, defh, defh, defh, defh,
  /* F0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
};
static process_command_t patch_server_handlers[0x100] = {
  /* 00 */ defh, defh, process_server_pc_gc_patch_02_17, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 10 */ defh, defh, defh, defh, process_server_game_19_patch_14, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 20 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 30 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 40 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 50 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 60 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 70 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 80 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 90 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* A0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* B0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* C0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* D0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* E0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* F0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
};



static process_command_t dc_client_handlers[0x100] = {
  /* 00 */ defh, defh, defh, defh, defh, defh, process_client_06, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 10 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 20 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 30 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 40 */ process_client_40, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 50 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 60 */ process_client_60_62_6C_6D_C9_CB<void>, defh, process_client_60_62_6C_6D_C9_CB<void>, defh, defh, defh, defh, defh, defh, defh, defh, defh, process_client_60_62_6C_6D_C9_CB<void>, process_client_60_62_6C_6D_C9_CB<void>, defh, defh,
  /* 70 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 80 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 90 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* A0 */ process_client_dc_pc_gc_A0_A1, process_client_dc_pc_gc_A0_A1, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* B0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* C0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* D0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* E0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* F0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
};
static process_command_t pc_client_handlers[0x100] = {
  /* 00 */ defh, defh, defh, defh, defh, defh, process_client_06, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 10 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 20 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 30 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 40 */ process_client_40, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 50 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 60 */ process_client_60_62_6C_6D_C9_CB<void>, defh, process_client_60_62_6C_6D_C9_CB<void>, defh, defh, defh, defh, defh, defh, defh, defh, defh, process_client_60_62_6C_6D_C9_CB<void>, process_client_60_62_6C_6D_C9_CB<void>, defh, defh,
  /* 70 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 80 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 90 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* A0 */ process_client_dc_pc_gc_A0_A1, process_client_dc_pc_gc_A0_A1, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* B0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* C0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* D0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* E0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* F0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
};
static process_command_t gc_client_handlers[0x100] = {
  /* 00 */ defh, defh, defh, defh, defh, defh, process_client_06, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 10 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 20 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 30 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 40 */ process_client_40, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 50 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 60 */ process_client_60_62_6C_6D_C9_CB<G_SendGuildCard_GC_6x06>, defh, process_client_60_62_6C_6D_C9_CB<G_SendGuildCard_GC_6x06>, defh, defh, defh, defh, defh, defh, defh, defh, defh, process_client_60_62_6C_6D_C9_CB<G_SendGuildCard_GC_6x06>, process_client_60_62_6C_6D_C9_CB<G_SendGuildCard_GC_6x06>, defh, defh,
  /* 70 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 80 */ defh, process_client_81<SC_SimpleMail_GC_81>, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 90 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* A0 */ process_client_dc_pc_gc_A0_A1, process_client_dc_pc_gc_A0_A1, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* B0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* C0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* D0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* E0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* F0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
};
static process_command_t bb_client_handlers[0x100] = {
  /* 00 */ defh, defh, defh, defh, defh, defh, process_client_06, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 10 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 20 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 30 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 40 */ process_client_40, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 50 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 60 */ process_client_60_62_6C_6D_C9_CB<G_SendGuildCard_BB_6x06>, defh, process_client_60_62_6C_6D_C9_CB<G_SendGuildCard_BB_6x06>, defh, defh, defh, defh, defh, defh, defh, defh, defh, process_client_60_62_6C_6D_C9_CB<G_SendGuildCard_BB_6x06>, process_client_60_62_6C_6D_C9_CB<G_SendGuildCard_BB_6x06>, defh, defh,
  /* 70 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 80 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 90 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* A0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* B0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* C0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* D0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* E0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* F0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
};
static process_command_t patch_client_handlers[0x100] = {
  /* 00 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 10 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 20 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 30 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 40 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 50 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 60 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 70 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 80 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* 90 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* A0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* B0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* C0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* D0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* E0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
  /* F0 */ defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh, defh,
};



static process_command_t* server_handlers[] = {
    dc_server_handlers, pc_server_handlers, patch_server_handlers, gc_server_handlers, bb_server_handlers};
static process_command_t* client_handlers[] = {
    dc_client_handlers, pc_client_handlers, patch_client_handlers, gc_client_handlers, bb_client_handlers};

static process_command_t get_handler(GameVersion version, bool from_server, uint8_t command) {
  size_t version_index = static_cast<size_t>(version);
  if (version_index >= 5) {
    throw logic_error("invalid game version on proxy server");
  }
  return (from_server ? server_handlers : client_handlers)[version_index][command];
}

void process_proxy_command(
    shared_ptr<ServerState> s,
    ProxyServer::LinkedSession& session,
    bool from_server,
    uint16_t command,
    uint32_t flag,
    string& data) {
  auto fn = get_handler(session.version, from_server, command);
  try {
    bool should_forward = fn(s, session, command, flag, data);
    if (should_forward) {
      forward_command(session, !from_server, command, flag, data);
    }
  } catch (const exception& e) {
    session.log(ERROR, "Failed to process command: %s", e.what());
    session.disconnect();
  }
}
