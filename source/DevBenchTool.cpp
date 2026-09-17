#include "PCH.h"

#include "DevBenchTool.h"

#include "DevBench/DevBenchAPI.h"
#include "ModelSync.h"
#include "utils/Logger.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace DevBenchTool
{
	namespace
	{
		std::string EscapeJson(std::string_view a_in)
		{
			std::string out;
			out.reserve(a_in.size() + 8);
			for (const char c : a_in)
			{
				switch (c)
				{
				case '\\': out += "\\\\"; break;
				case '"': out += "\\\""; break;
				case '\n': out += "\\n"; break;
				case '\r': break;
				default: out += c; break;
				}
			}
			return out;
		}

		std::string Get(std::string_view a_json, const char* a_name)
		{
			const std::string key = std::format("\"{}\"", a_name);
			auto pos = a_json.find(key);
			if (pos == std::string_view::npos) { return {}; }
			pos = a_json.find(':', pos + key.size());
			if (pos == std::string_view::npos) { return {}; }
			++pos;
			while (pos < a_json.size() && (a_json[pos] == ' ' || a_json[pos] == '\t')) { ++pos; }
			if (pos >= a_json.size()) { return {}; }
			if (a_json[pos] == '"')
			{
				std::string out;
				for (++pos; pos < a_json.size() && a_json[pos] != '"'; ++pos)
				{
					if (a_json[pos] == '\\' && pos + 1 < a_json.size()) { ++pos; }
					out += a_json[pos];
				}
				return out;
			}
			std::string out;
			while (pos < a_json.size() && a_json[pos] != ',' && a_json[pos] != '}' && a_json[pos] != ' ') { out += a_json[pos++]; }
			return out;
		}

		// DevBench calls on its own thread; everything that touches the menu runs on the game's.
		bool RunOnMainThread(std::function<void()> a_fn, int a_timeoutMs = 8000)
		{
			auto* tasks = SKSE::GetTaskInterface();
			if (!tasks) { return false; }
			auto done = std::make_shared<std::atomic<bool>>(false);
			auto m = std::make_shared<std::mutex>();
			auto cv = std::make_shared<std::condition_variable>();
			tasks->AddTask([=]() {
				a_fn();
				{
					std::scoped_lock l(*m);
					done->store(true);
				}
				cv->notify_all();
			});
			std::unique_lock l(*m);
			return cv->wait_for(l, std::chrono::milliseconds(a_timeoutMs), [&]() { return done->load(); });
		}

		void Tool(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
		{
			const std::string_view args = a_argsJson ? a_argsJson : "";
			const std::string op = Get(args, "op");

			if (op == "next")
			{
				// Ask the movie for another hint exactly as Loading Menu Overhaul's button does; the
				// sync then sees the new text on the next AdvanceMovie and swaps the model.
				bool ok = false;
				bool viaTask = true;
				if (!RunOnMainThread([&]() { ok = modelsync::RequestNextHint(); }, 1500))
				{
					// During a load the game's task queue may not be serviced; the movie invoke is a
					// single Scaleform call, so make it directly rather than report nothing.
					viaTask = false;
					logger::warn("op=next: the task queue did not run within 1.5 s; invoking the movie directly from the DevBench thread");
					ok = modelsync::RequestNextHint();
				}
				(void)viaTask;
				a_write(a_sink, std::format("{{\"ok\":{},\"op\":\"next\",{}}}", ok ? "true" : "false", modelsync::StateJson()).c_str());
				return;
			}
			if (op == "apply")
			{
				// Force a particular load screen's model: formid is a whole FormID in hex.
				const std::string id = Get(args, "formid");
				std::uint32_t formID = 0;
				try { formID = static_cast<std::uint32_t>(std::stoul(id, nullptr, 16)); } catch (...) {}
				if (!formID)
				{
					a_write(a_sink, R"json({"ok":false,"op":"apply","error":"need formid, in hex"})json");
					return;
				}
				bool ok = false;
				RunOnMainThread([&]() {
					auto* screen = RE::TESForm::LookupByID<RE::TESLoadScreen>(formID);
					ok = screen && modelsync::ApplyScreen(screen, "devbench");
				});
				a_write(a_sink, std::format("{{\"ok\":{},\"op\":\"apply\",\"formid\":\"{:08X}\",{}}}", ok ? "true" : "false", formID, modelsync::StateJson()).c_str());
				return;
			}
			if (op == "set")
			{
				const std::string name = Get(args, "name");
				const std::string value = Get(args, "value");
				const bool on = value == "1" || value == "true" || value == "yes";
				if (name == "enabled") { modelsync::GetSettings().enabled = on; }
				else
				{
					a_write(a_sink, std::format("{{\"ok\":false,\"op\":\"set\",\"error\":\"unknown setting \\\"{}\\\"\"}}", EscapeJson(name)).c_str());
					return;
				}
				a_write(a_sink, std::format("{{\"ok\":true,\"op\":\"set\",\"name\":\"{}\",\"value\":{}}}", EscapeJson(name), on ? "true" : "false").c_str());
				return;
			}

			std::string state;
			RunOnMainThread([&]() { state = modelsync::StateJson(); }, 2000);
			if (state.empty()) { state = modelsync::StateJson(); }
			a_write(a_sink, std::format("{{\"ok\":true,\"op\":\"state\",{}}}", state).c_str());
		}
	}

	void Init(bool a_lastAttempt)
	{
		static bool registered = false;
		if (registered) { return; }

		DevBenchAPI::IDevBenchInterface001* devBench = DevBenchAPI::GetDevBenchInterface001();
		if (!devBench)
		{
			if (a_lastAttempt) { logger::info("DevBench not detected; skipping the \"lmo.modelsync\" tool"); }
			else { logger::debug("DevBench not detected yet; will retry at the next message"); }
			return;
		}

		constexpr const char* descriptor =
			"{"
			"\"description\":\"Loading Menu Overhaul - Model Sync. op=state (default): whether the hook is installed, "
			"the engine function resolved, the hint text last seen, the load screen whose model is up, and counts of "
			"swaps applied and retried. op=next: while the Loading Menu is open, ask the movie for the next hint the way "
			"Loading Menu Overhaul's swipe does, so the sync swaps the model. op=apply with formid (hex): force that load "
			"screen's model. op=set with name=enabled and value: switch the sync for this session.\","
			"\"inputSchema\":{\"type\":\"object\",\"properties\":{\"op\":{\"type\":\"string\"},\"formid\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"},\"value\":{\"type\":\"string\"}}},"
			"\"readOnly\":false"
			"}";

		if (devBench->RegisterTool("lmo.modelsync", descriptor, &Tool, nullptr))
		{
			logger::info("Registered \"lmo.modelsync\" with DevBench (build {})", devBench->GetBuildNumber());
			registered = true;
		}
	}
}
