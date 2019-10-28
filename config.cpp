#include "bot.h"
#include "config.h"
#include "database.h"
#include "stringops.h"
#include "help.h"
#include <string>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <cstdint>
#include <mutex>
#include <stdlib.h>

using namespace SleepyDiscord;

std::mutex config_sql_mutex;

void DoConfigSet(class Bot* bot, std::stringstream &param, const std::string &channelID, SleepyDiscord::User issuer);
void DoConfigIgnore(class Bot* bot, std::stringstream &param, const std::string &channelID, SleepyDiscord::Message message);
void DoConfigShow(class Bot* bot, const std::string &channelID, SleepyDiscord::User issuer);

rapidjson::Document getSettings(Bot* bot, const std::string &channel_id, const std::string& guild_id)
{
	Channel channel;

	ChannelCache::iterator iter = bot->channelList.find(channel_id);
	if (iter != bot->channelList.end()) {
		channel = iter->second;
	} else {
		const Channel& c = bot->getChannel(Snowflake<Channel>(channel_id)).cast();
		bot->channelList[channel_id] = c;
		channel = c;
	}

	return getSettings(bot, channel, guild_id);
}

rapidjson::Document getSettings(Bot* bot, const Channel& channel, const std::string& guild_id)
{
	std::lock_guard<std::mutex> sql_lock(config_sql_mutex);
	rapidjson::Document settings;
	std::string cid = std::string(channel.ID);

	/* DM channel */
	if (channel.name == "") {
		settings.Parse("{}");
		return settings;
	}

	/* Retrieve from db */
	db::resultset r = db::query("SELECT settings, parent_id, name FROM infobot_discord_settings WHERE id = ?", {cid});

	std::string parent_id = std::string(channel.parentID);
	std::string name = channel.name;
	if (parent_id == "") {
		parent_id = "NULL";
	}

	if (channel.type == 0) {	/* Channel type: TEXT */
		name = std::string("#") + name;
	}

	if (r.empty()) {
		/* No settings for this channel, create an entry */
		db::query("INSERT INTO infobot_discord_settings (id, parent_id, guild_id, name, settings) VALUES(?, ?, ?, '?', '?')", {cid, parent_id, guild_id, name, std::string("{}")});
		r = db::query("SELECT settings FROM infobot_discord_settings WHERE id = ?", {cid});

	} else if (name != r[0].find("name")->second || parent_id != r[0].find("parent_id")->second) {
		/* Data has changed, run update query */
		db::query("UPDATE infobot_discord_settings SET parent_id = ?, name = '?' WHERE id = ?", {parent_id, name, cid});
	}

	db::row row = r[0];
	std::string json = row.find("settings")->second;
	settings.Parse(json.c_str());

	if (settings.IsObject()) {
		return settings;
	} else {
		settings.Parse("{}");
		return settings;
	}
}

// {"talkative":true,"learningdisabled":false,"ignores":[159985870458322944,155149108183695360]}

namespace settings {

	bool IsLearningDisabled(const rapidjson::Document& settings) {
		return (settings.IsObject() && settings.HasMember("learningdisabled") && settings["learningdisabled"].IsBool() && settings["learningdisabled"].GetBool() == true);
	}

	bool IsLearningEnabled(const rapidjson::Document& settings) {
		return !IsLearningDisabled(settings);
	}

	bool IsTalkative(const rapidjson::Document& settings) {
		return (settings.IsObject() && settings.HasMember("talkative") && settings["talkative"].IsBool() && settings["talkative"].GetBool() == true);
	}

	std::vector<uint64_t> GetIgnoreList(const rapidjson::Document& settings) {
		std::vector<uint64_t> ignores;
		if (settings.IsObject() && settings.HasMember("ignores") && settings["ignores"].IsArray()) {
			for (size_t i = 0; i < settings["ignores"].Size(); ++i) {
				ignores.push_back(settings["ignores"][i].GetInt64());
			}
		}
		return ignores;
	}

	std::string GetIgnoreList(const rapidjson::Document& settings, bool as_json) {
		std::string json = "[";
		if (settings.IsObject() && settings.HasMember("ignores") && settings["ignores"].IsArray()) {
			for (size_t i = 0; i < settings["ignores"].Size(); ++i) {
				json.append(std::to_string(settings["ignores"][i].GetInt64()));
				if (i != settings["ignores"].Size() - 1) {
					json.append(",");
				}
			}
		}
		json.append("]");
		return json;
	}
};

void DoConfig(class Bot* bot, const std::vector<std::string> &param, const std::string &channelID, SleepyDiscord::Message message) {

	if (!HasPermission(bot, channelID, message)) {
		GetHelp(bot, "access-denied", channelID, bot->user.username, std::string(bot->getID()), message.author.username, message.author.ID, false);
		return;
	}

	if (param.size() < 3) {
		GetHelp(bot, "missing-parameters", channelID, bot->user.username, std::string(bot->getID()), message.author.username, message.author.ID, false);
		return;
	}
	std::stringstream tokens(trim(param[2]));
	std::string subcommand;
	tokens >> subcommand;
	if (subcommand == "show") {
		DoConfigShow(bot, channelID, message.author);
	} else if (subcommand == "ignore") {
		DoConfigIgnore(bot, tokens, channelID, message);
	} else if (subcommand == "set") {
		DoConfigSet(bot, tokens, channelID, message.author);
	}
}

void EmbedSimple(Bot* bot, const std::string &message, const std::string &channelID) {
	std::stringstream s;
	s << "{\"color\":16767488, \"description\": \"" << message << "\"}";
	SleepyDiscord::Embed embed(s.str());
	bot->sendMessage(channelID, "", embed, false);
}

void DoConfigSet(class Bot* bot, std::stringstream &param, const std::string &channelID, SleepyDiscord::User issuer) {
	std::string variable;
	std::string setting;
	param >> variable >> setting;
	variable = lowercase(variable);
	setting = lowercase(setting);
	if (variable == "" || setting == "") {
		GetHelp(bot, "missing-set-var-or-value", channelID, bot->user.username, std::string(bot->getID()), issuer.username, issuer.ID, false);
		return;
	}
	if (variable != "talkative" && variable != "learn") {
		GetHelp(bot, "invalid-set-var-or-value", channelID, bot->user.username, std::string(bot->getID()), issuer.username, issuer.ID, false);
		return;
	}
	bool state = (setting == "yes" || setting == "true" || setting == "on" || setting == "1");
	rapidjson::Document csettings = getSettings(bot, channelID, "");

	bool talkative = (variable == "talkative" ? state : settings::IsTalkative(csettings));
	bool learningdisabled = (variable == "learn" ? !state : settings::IsLearningDisabled(csettings));
	bool outstate = state;
	if (variable == "learn") {
		outstate = !state;
	}

	std::string json = json::createJSON({
		{ "talkative", json::boolean(talkative) },
		{ "learningdisabled", json::boolean(learningdisabled) },
		{ "ignores", settings::GetIgnoreList(csettings, true) }
	});

	db::query("UPDATE infobot_discord_settings SET settings = '?' WHERE id = ?", {json, channelID});

	EmbedSimple(bot, "Setting **'" + variable + "'** " + (outstate ? "enabled" : "disabled") + " on <#" + channelID + ">", channelID);
}

std::string ToJSON(std::vector<uint64_t> list) {
	std::string ret = "[";
	for (auto i = list.begin(); i != list.end(); ++i) {
		ret += std::to_string(*i) + ",";
	}
	ret = ret.substr(0, ret.length() - 1) + "]";
	return ret;
}

bool HasPermission(class Bot* bot, const std::string &channelID, SleepyDiscord::Message message) {
	std::string serverID = message.serverID;
	BotServerCache::iterator s = bot->serverList.find(serverID);
	if (s != bot->serverList.end()) {
		if (s->second.ownerID == message.author.ID) {
			/* Server owner */
			return true;
		}
		for (auto serverrole = s->second.roles.begin(); serverrole != s->second.roles.end(); ++serverrole) {
			for (auto memberrole = message.member.roles.begin(); memberrole != message.member.roles.end(); ++memberrole) {
				/* Manage messages or administrator permission */
				if (serverrole->ID == *memberrole) {
					if ((serverrole->permissions & SleepyDiscord::Permission::MANAGE_MESSAGES) || (serverrole->permissions & SleepyDiscord::Permission::ADMINISTRATOR)) {
						return true;
					}
				}
			}
		}
	}
	return false;
}

void DoConfigIgnore(class Bot* bot, std::stringstream &param, const std::string &channelID, SleepyDiscord::Message message) {
	rapidjson::Document csettings = getSettings(bot, channelID, "");
	std::string operation;
	param >> operation;
	std::string userlist;
	std::vector<uint64_t> currentlist = settings::GetIgnoreList(csettings);
	std::vector<uint64_t> mentions;
	for (auto i = message.mentions.begin(); i != message.mentions.end(); ++i) {
		if (i->ID != bot->getID()) {
			mentions.push_back(from_string<uint64_t>(i->ID, std::dec));
			userlist += " " + i->username;
		}
	}
	if (operation == "add") {
		for (auto i = mentions.begin(); i != mentions.end(); ++i) {
			currentlist.push_back(*i);
		}
		std::string json = json::createJSON({
			{ "talkative", json::boolean(settings::IsTalkative(csettings)) },
			{ "learningdisabled", json::boolean(settings::IsLearningDisabled(csettings)) },
			{ "ignores", ToJSON(currentlist) },
		});
		db::query("UPDATE infobot_discord_settings SET settings = '?' WHERE id = ?", {json, channelID});
		EmbedSimple(bot, std::string("Added **") + std::to_string(mentions.size()) + " user" + (mentions.size() > 1 ? "s" : "") + "** to the ignore list for <#" + channelID + ">: " + userlist, channelID);
	} else if (operation == "del") {
		std::vector<uint64_t> newlist;
		for (auto i = currentlist.begin(); i != currentlist.end(); ++i) {
			bool preserve = true;
			for (auto j = mentions.begin(); j != mentions.end(); ++j) {
				if (*j == *i) {
					preserve = false;
					break;
				}
			}
			if (preserve) {
				newlist.push_back(*i);
			}
		}
		currentlist = newlist;
		std::string json = json::createJSON({
			{ "talkative", json::boolean(settings::IsTalkative(csettings)) },
			{ "learningdisabled", json::boolean(settings::IsLearningDisabled(csettings)) },
			{ "ignores", ToJSON(currentlist) },
		});
		db::query("UPDATE infobot_discord_settings SET settings = '?' WHERE id = ?", {json, channelID});
		EmbedSimple(bot, std::string("Deleted **") + std::to_string(mentions.size()) + " user" + (mentions.size() > 1 ? "s" : "") + "** from the ignore list for <#" + channelID + ">: " + userlist, channelID);
	} else if (operation == "list") {
		std::stringstream s;
		if (currentlist.empty()) {
			s << "**Ignore list for <#" << channelID << "> is empty!**";
		} else {
			s << "**Ignore list for <#" << channelID << ">**\\n\\n";
			for (auto i = currentlist.begin(); i != currentlist.end(); ++i) {
				s << "<@" << *i << "> (" << *i << ")\\n";
			}
		}
		EmbedSimple(bot, s.str(), channelID);
	}
}

void DoConfigShow(class Bot* bot, const std::string &channelID, SleepyDiscord::User issuer) {
	rapidjson::Document csettings = getSettings(bot, channelID, "");
	std::stringstream s;
	const statusfield statusfields[] = {
		statusfield("Talk without being mentioned?", settings::IsTalkative(csettings) ? "Yes" : "No"),
		statusfield("Learn from this channel?", settings::IsLearningEnabled(csettings) ? "Yes" : "No"),
		statusfield("Ignored users", Comma(settings::GetIgnoreList(csettings).size())),
		statusfield("", "")
	};
	s << "{\"title\":\"Settings for this channel\",\"color\":16767488,";
	s << "\"footer\":{\"link\":\"https;\\/\\/www.botnix.org\\/\",\"text\":\"Powered by Botnix 2.0 with the infobot and discord modules\",\"icon_url\":\"https:\\/\\/www.botnix.org\\/images\\/botnix.png\"},\"fields\":[";
	for (int i = 0; statusfields[i].name != ""; ++i) {
		s << "{\"name\":\"" +  statusfields[i].name + "\",\"value\":\"" + statusfields[i].value + "\", \"inline\": false}";
		if (statusfields[i + 1].name != "") {
			s << ",";
		}
	}
	s << "],\"description\":\"For help on changing these settings, type ``@" << bot->user.username << " help config``.\"}";
	SleepyDiscord::Embed embed(s.str());
	bot->sendMessage(channelID, "", embed, false);
}
