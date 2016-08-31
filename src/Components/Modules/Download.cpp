#include "STDInclude.hpp"

namespace Components
{
	mg_mgr Download::Mgr;
	Download::ClientDownload Download::CLDownload;

#pragma region Client

	void Download::InitiateClientDownload(std::string mod)
	{
		if (Download::CLDownload.Running) return;

		Download::CLDownload.Mutex.lock();
		Download::CLDownload.Progress = "Downloading mod";
		Download::CLDownload.Mutex.unlock();

		Command::Execute("openmenu popup_reconnectingtoparty", true);

		Download::CLDownload.Running = true;
		Download::CLDownload.Mod = mod;
		Download::CLDownload.TerminateThread = false;
		Download::CLDownload.Target = Party::Target();
		Download::CLDownload.Thread = std::thread(Download::ModDownloader, &Download::CLDownload);
	}

	bool Download::ParseModList(ClientDownload* download, std::string list)
	{
		if (!download) return false;
		download->Files.clear();

		std::string error;
		json11::Json listData = json11::Json::parse(list, error);


		if (!error.empty() || !listData.is_array()) return false;

		for (auto& file : listData.array_items())
		{
			if (!file.is_object()) return false;

			auto hash = file["hash"];
			auto name = file["name"];
			auto size = file["size"];

			if (!hash.is_string() || !name.is_string() || !size.is_number()) return false;

			Download::ClientDownload::File fileEntry;
			fileEntry.Name = name.string_value();
			fileEntry.Hash = hash.string_value();
			fileEntry.Size = static_cast<size_t>(size.number_value());

			if (!fileEntry.Name.empty())
			{
				download->Files.push_back(fileEntry);
			}
		}

		return true;
	}

	void Download::DownloadHandler(mg_connection *nc, int ev, void* ev_data)
	{
		http_message* hm = reinterpret_cast<http_message*>(ev_data);
		Download::FileDownload* fDownload = reinterpret_cast<Download::FileDownload*>(nc->mgr->user_data);

		if (ev == MG_EV_CONNECT)
		{
			if (hm->message.p)
			{
				fDownload->downloading = false;
				return;
			}
		}

		if (ev == MG_EV_RECV)
		{
			fDownload->receivedBytes += static_cast<size_t>(*reinterpret_cast<int*>(ev_data));

			double progress = (100.0 / fDownload->file.Size) * fDownload->receivedBytes;
			fDownload->download->Mutex.lock();
			fDownload->download->Progress = fmt::sprintf("Downloading file (%d/%d) %s %d%%", fDownload->index + 1, fDownload->download->Files.size(), fDownload->file.Name.data(), static_cast<unsigned int>(progress));
			fDownload->download->Mutex.unlock();
		}

		if (ev == MG_EV_HTTP_REPLY)
		{
			nc->flags |= MG_F_CLOSE_IMMEDIATELY;
			fDownload->buffer = std::string(hm->body.p, hm->body.len);
			fDownload->downloading = false;
			return;
		}
	}

	bool Download::DownloadFile(ClientDownload* download, unsigned int index)
	{
		if (!download || download->Files.size() <= index) return false;

		auto file = download->Files[index];

		download->Mutex.lock();
		download->Progress = fmt::sprintf("Downloading file (%d/%d) %s 0%%", index + 1, download->Files.size(), file.Name.data());
		download->Mutex.unlock();

		std::string path = download->Mod + "/" + file.Name;
		if (Utils::IO::FileExists(path))
		{
			std::string data = Utils::IO::ReadFile(path);

			if (data.size() == file.Size && Utils::String::DumpHex(Utils::Cryptography::SHA256::Compute(data), "") == file.Hash)
			{
				return true;
			}
		}

		std::string url = "http://" + download->Target.GetString() + "/file/" + file.Name;

		Download::FileDownload fDownload;
		fDownload.file = file;
		fDownload.index = index;
		fDownload.download = download;
		fDownload.downloading = true;
		fDownload.receivedBytes = 0;

		download->Valid = true;
		mg_mgr_init(&download->Mgr, &fDownload);
		mg_connect_http(&download->Mgr, Download::DownloadHandler, url.data(), NULL, NULL);

		while (fDownload.downloading && !fDownload.download->TerminateThread)
		{
			mg_mgr_poll(&download->Mgr, 0);
		}

		mg_mgr_free(&download->Mgr);
		download->Valid = false;

		if (fDownload.buffer.size() != file.Size || Utils::String::DumpHex(Utils::Cryptography::SHA256::Compute(fDownload.buffer), "") != file.Hash)
		{
			return false;
		}

		Utils::IO::CreateDirectory(download->Mod);
		Utils::IO::WriteFile(path, fDownload.buffer);

		return true;
	}

	void Download::ModDownloader(ClientDownload* download)
	{
		if (!download) download = &Download::CLDownload;

		std::string host = "http://" + download->Target.GetString();
		std::string list = Utils::WebIO("IW4x", host + "/list").SetTimeout(5000)->Get();

		if (list.empty())
		{
			download->Thread.detach();
			download->Clear();

			QuickPatch::Once([] ()
			{
				Party::ConnectError("Failed to download the modlist!");
			});

			return;
		}

		if (download->TerminateThread) return;

		if (!Download::ParseModList(download, list))
		{
			download->Thread.detach();
			download->Clear();

			QuickPatch::Once([] ()
			{
				Party::ConnectError("Failed to parse the modlist!");
			});

			return;
		}

		if (download->TerminateThread) return;

		for (unsigned int i = 0; i < download->Files.size(); ++i)
		{
			if (download->TerminateThread) return;

			if (!Download::DownloadFile(download, i))
			{
				Dvar::Var("partyend_reason").Set(fmt::sprintf("Failed to download file: %s!", download->Files[i].Name.data()));

				download->Thread.detach();
				download->Clear();

				QuickPatch::Once([] ()
				{
					Localization::ClearTemp();
					Command::Execute("closemenu popup_reconnectingtoparty");
					Command::Execute("openmenu menu_xboxlive_partyended");
				});

				return;
			}
		}

		if (download->TerminateThread) return;

		static std::string mod = download->Mod;

		download->Thread.detach();
		download->Clear();
		
		// Run this on the main thread
		QuickPatch::Once([] ()
		{
			auto fsGame = Dvar::Var("fs_game");
			fsGame.Set(mod);
			fsGame.Get<Game::dvar_t*>()->modified = true;

			mod.clear();

			Localization::ClearTemp();
			Command::Execute("closemenu popup_reconnectingtoparty", true);

			if (Dvar::Var("cl_modVidRestart").Get<bool>())
			{
				Command::Execute("vid_restart", false);
			}

			Command::Execute("reconnect", false);
		});
	}

#pragma endregion

#pragma region Server

	bool Download::IsClient(mg_connection *nc)
	{
		return (Download::GetClient(nc) != nullptr);
	}

	Game::client_t* Download::GetClient(mg_connection *nc)
	{
		Network::Address address(nc->sa.sa);

		for (int i = 0; i < *Game::svs_numclients; ++i)
		{
			Game::client_t* client = &Game::svs_clients[i];

			if (client->state >= 3)
			{
				if (address.GetIP().full == Network::Address(client->addr).GetIP().full)
				{
					return client;
				}
			}
		}

		return nullptr;
	}

	void Download::Forbid(mg_connection *nc)
	{
		mg_printf(nc, "HTTP/1.1 403 Forbidden\r\n"
			"Content-Type: text/html\r\n"
			"Connection: close\r\n"
			"\r\n"
			"403 - Forbidden");

		nc->flags |= MG_F_SEND_AND_CLOSE;
	}

	void Download::ListHandler(mg_connection* nc, int ev, void* /*ev_data*/)
	{
		// Only handle http requests
		if (ev != MG_EV_HTTP_REQUEST) return;

// 		if (!Download::IsClient(nc))
// 		{
// 			Download::Forbid(nc);
// 		}
// 		else
		{
			static std::string fsGamePre;
			static json11::Json jsonList;

			std::string fsGame = Dvar::Var("fs_game").Get<std::string>();
			
			if (!fsGame.empty() && fsGame != fsGamePre)
			{
				std::vector<json11::Json> fileList;

				fsGamePre = fsGame;

				std::string path = Dvar::Var("fs_basepath").Get<std::string>() + "\\" + fsGame;
				auto list = FileSystem::GetSysFileList(path, "iwd", false);

				list.push_back("mod.ff");

				for (auto i = list.begin(); i != list.end(); ++i)
				{
					std::string filename = path + "\\" + *i;
					if (strstr(i->data(), "_svr_") == NULL && Utils::IO::FileExists(filename))
					{
						std::map<std::string, json11::Json> file;
						std::string fileBuffer = Utils::IO::ReadFile(path + "\\" + *i);

						file["name"] = *i;
						file["size"] = static_cast<int>(fileBuffer.size());
						file["hash"] = Utils::Cryptography::SHA256::Compute(fileBuffer, true);

						fileList.push_back(file);
					}
				}

				jsonList = fileList;
			}

			mg_printf(nc,
				"HTTP/1.1 200 OK\r\n"
				"Content-Type: application/json\r\n"
				"Connection: close\r\n"
				"\r\n"
				"%s", jsonList.dump().data());

			nc->flags |= MG_F_SEND_AND_CLOSE;
		}
	}

	void Download::FileHandler(mg_connection *nc, int ev, void *ev_data)
	{
		// Only handle http requests
		if (ev != MG_EV_HTTP_REQUEST) return;

		http_message* message = reinterpret_cast<http_message*>(ev_data);

// 		if (!Download::IsClient(nc))
// 		{
// 			Download::Forbid(nc);
// 		}
// 		else
		{
			std::string url(message->uri.p, message->uri.len);
			Utils::String::Replace(url, "\\", "/");
			url = url.substr(6);

			if (url.find_first_of("/") != std::string::npos || (!Utils::String::EndsWith(url, ".iwd") && url != "mod.ff") || strstr(url.data(), "_svr_") != NULL)
			{
				Download::Forbid(nc);
				return;
			}

			std::string fsGame = Dvar::Var("fs_game").Get<std::string>();
			std::string path = Dvar::Var("fs_basepath").Get<std::string>() + "\\" + fsGame + "\\" + url;

			if (fsGame.empty() || !Utils::IO::FileExists(path))
			{
				mg_printf(nc,
					"HTTP/1.1 404 Not Found\r\n"
					"Content-Type: text/html\r\n"
					"Connection: close\r\n"
					"\r\n"
					"404 - Not Found %s", path.data());
			}
			else
			{
				std::string file = Utils::IO::ReadFile(path);

				mg_printf(nc,
					"HTTP/1.1 200 OK\r\n"
					"Content-Type: application/octet-stream\r\n"
					"Content-Length: %d\r\n"
					"Connection: close\r\n"
					"\r\n", file.size());

				mg_send(nc, file.data(), static_cast<int>(file.size()));
			}

			nc->flags |= MG_F_SEND_AND_CLOSE;
		}
	}

	void Download::InfoHandler(mg_connection* nc, int ev, void* /*ev_data*/)
	{
		// Only handle http requests
		if (ev != MG_EV_HTTP_REQUEST) return;

		//http_message* message = reinterpret_cast<http_message*>(ev_data);

		Utils::InfoString status = ServerInfo::GetInfo();

		std::map<std::string, json11::Json> info;
		info["status"] = status.to_json();

		std::vector<json11::Json> players;

		// Build player list
		for (int i = 0; i < atoi(status.Get("sv_maxclients").data()); ++i) // Maybe choose 18 here? 
		{
			std::map<std::string, json11::Json> playerInfo;
			playerInfo["score"] = 0;
			playerInfo["ping"] = 0;
			playerInfo["name"] = "";

			if (Dvar::Var("sv_running").Get<bool>())
			{
				if (Game::svs_clients[i].state < 3) continue;

				playerInfo["score"] = Game::SV_GameClientNum_Score(i);
				playerInfo["ping"] = Game::svs_clients[i].ping;
				playerInfo["name"] = Game::svs_clients[i].name;
			}
			else
			{
				// Score and ping are irrelevant
				const char* namePtr = Game::PartyHost_GetMemberName(reinterpret_cast<Game::PartyData_t*>(0x1081C00), i);
				if (!namePtr || !namePtr[0]) continue;

				playerInfo["name"] = namePtr;
			}

			players.push_back(playerInfo);
		}

		info["players"] = players;

		mg_printf(nc,
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: application/json\r\n"
			"Connection: close\r\n"
			"Access-Control-Allow-Origin: *\r\n"
			"\r\n"
			"%s", json11::Json(info).dump().data());

		nc->flags |= MG_F_SEND_AND_CLOSE;
	}

	void Download::EventHandler(mg_connection *nc, int ev, void *ev_data)
	{
		// Only handle http requests
		if (ev != MG_EV_HTTP_REQUEST) return;

		http_message* message = reinterpret_cast<http_message*>(ev_data);

// 		if (message->uri.p, message->uri.len == "/"s)
// 		{
// 			mg_printf(nc, 
// 				"HTTP/1.1 200 OK\r\n"
// 				"Content-Type: text/html\r\n"
// 				"Connection: close\r\n"
// 				"\r\n"
// 				"Hi fella!<br>You are%s connected to this server!", (Download::IsClient(nc) ? " " : " not"));
// 
// 			Game::client_t* client = Download::GetClient(nc);
// 
// 			if (client)
// 			{
// 				mg_printf(nc, "<br>Hello %s!", client->name);
// 			}
// 		}
// 		else
		{
			//std::string path = (Dvar::Var("fs_basepath").Get<std::string>() + "\\" BASEGAME "\\html");
			//mg_serve_http_opts opts = { 0 };
			//opts.document_root = path.data();
			//mg_serve_http(nc, message, opts);

			FileSystem::File file;
			std::string url = "html" + std::string(message->uri.p, message->uri.len);

			if (Utils::String::EndsWith(url, "/"))
			{
				url.append("index.html");
				file = FileSystem::File(url);
			}
			else
			{
				file = FileSystem::File(url);

				if (!file.Exists())
				{
					url.append("/index.html");
					file = FileSystem::File(url);
				}
			}

			std::string mimeType = Utils::GetMimeType(url);

			if (file.Exists())
			{
				std::string& buffer = file.GetBuffer();

				mg_printf(nc,
					"HTTP/1.1 200 OK\r\n"
					"Content-Type: %s\r\n"
					"Content-Length: %d\r\n"
					"Connection: close\r\n"
					"\r\n", mimeType.data(), buffer.size());

				mg_send(nc, buffer.data(), static_cast<int>(buffer.size()));
			}
			else
			{
				mg_printf(nc,
					"HTTP/1.1 404 Not Found\r\n"
					"Content-Type: text/html\r\n"
					"Connection: close\r\n"
					"\r\n"
					"404 - Not Found");
			}
		}

		nc->flags |= MG_F_SEND_AND_CLOSE;
	}

#pragma endregion

	Download::Download()
	{
		if (Dedicated::IsEnabled())
		{
			mg_mgr_init(&Download::Mgr, NULL);

			Network::OnStart([] ()
			{
				mg_connection* nc = mg_bind(&Download::Mgr, Utils::String::VA("%hu", (Dvar::Var("net_port").Get<int>() & 0xFFFF)), Download::EventHandler);

				// Handle special requests
				mg_register_http_endpoint(nc, "/info", Download::InfoHandler);
				mg_register_http_endpoint(nc, "/list", Download::ListHandler);
				mg_register_http_endpoint(nc, "/file", Download::FileHandler);

				mg_set_protocol_http_websocket(nc);
			});

			QuickPatch::OnFrame([]
			{
				mg_mgr_poll(&Download::Mgr, 0);
			});
		}
		else
		{
// 			Utils::Hook(0x5AC6E9, [] ()
// 			{
// 				// TODO: Perform moddownload here
// 
// 				Game::CL_DownloadsComplete(0);
// 			}, HOOK_CALL).Install()->Quick();

			QuickPatch::OnFrame([]
			{
				if (Download::CLDownload.Running)
				{
					Download::CLDownload.Mutex.lock();
					Localization::SetTemp("MENU_RECONNECTING_TO_PARTY", Download::CLDownload.Progress);
					Download::CLDownload.Mutex.unlock();
				}
			});
		}
	}

	Download::~Download()
	{
		if (Dedicated::IsEnabled())
		{
			mg_mgr_free(&Download::Mgr);
		}
		else
		{
			Download::CLDownload.Clear();
		}
	}
}
