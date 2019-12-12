#include "bot.h"
#include "config.h"
#include "database.h"
#include "stringops.h"
#include <string>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <cstdint>
#include <mutex>
#include <stdlib.h>
#include "help.h"

std::mutex config_sql_mutex;

void DoConfigSet(class Bot* bot, std::stringstream &param, int64_t channelID, const aegis::gateway::objects::user &issuer);
void DoConfigIgnore(class Bot* bot, std::stringstream &param, int64_t channelID, const aegis::gateway::objects::message &message);
void DoConfigShow(class Bot* bot, int64_t channelID, const aegis::gateway::objects::user &issuer);

namespace settings {

bool channelHasJS(int64_t channel_id)
{
	db::resultset r = db::query("SELECT id FROM infobot_discord_javascript WHERE id = ?", {std::to_string(channel_id)});
	/* No javascript configuration for this channel */
	if (r.size() == 0) {
		return false;
	} else {
		return true;
	}
}

std::string getJSConfig(int64_t channel_id, std::string variable)
{
	db::resultset r = db::query("SELECT `" + variable + "` FROM infobot_discord_javascript WHERE id = ?", {std::to_string(channel_id)});
	if (r.size() == 0) {
		return "";
	} else {
		return r[0].find(variable)->second;
	}
}

void setJSConfig(int64_t channel_id, std::string variable, std::string value)
{
	db::resultset r = db::query("UPDATE infobot_discord_javascript SET `" + variable + "` = '?' WHERE id = ?", {value, std::to_string(channel_id)});
}

};

json getSettings(Bot* bot, int64_t channel_id, int64_t guild_id)
{
	std::lock_guard<std::mutex> sql_lock(config_sql_mutex);
	json settings = json::parse("{}");
	std::string cid = std::to_string(channel_id);

	aegis::channel* channel = bot->core.find_channel(channel_id);

	if (!channel) {
		bot->core.log->error("WTF, find_channel({}) returned nullptr!", channel_id);
		return settings;
	}

	/* DM channels dont have settings */
	if (channel->get_type() == aegis::gateway::objects::channel::channel_type::DirectMessage) {
		return settings;
	}

	/* Retrieve from db */
	db::resultset r = db::query("SELECT settings, parent_id, name FROM infobot_discord_settings WHERE id = ?", {cid});

	std::string parent_id = std::to_string(channel->get_parent_id().get());
	std::string name = channel->get_name();

	if (parent_id == "" || parent_id == "0") {
		parent_id = "NULL";
	}

	if (channel->get_type() == aegis::gateway::objects::channel::channel_type::Text) {
		name = std::string("#") + name;
	}

	if (r.empty()) {
		/* No settings for this channel, create an entry */
		db::query("INSERT INTO infobot_discord_settings (id, parent_id, guild_id, name, settings) VALUES(?, ?, ?, '?', '?')", {cid, parent_id, std::to_string(guild_id), name, std::string("{}")});
		r = db::query("SELECT settings FROM infobot_discord_settings WHERE id = ?", {cid});

	} else if (name != r[0].find("name")->second || parent_id != r[0].find("parent_id")->second) {
		/* Data has changed, run update query */
		db::query("UPDATE infobot_discord_settings SET parent_id = ?, name = '?' WHERE id = ?", {parent_id, name, cid});
	}

	db::row row = r[0];
	std::string j = row.find("settings")->second;
	try {
		settings = json::parse(j);
	} catch (const std::exception &e) {
		bot->core.log->error("Can't parse settings for channel {}, id {}, json settings were: {}", channel->get_name(), cid, j);
	}

	return settings;
}

namespace settings {

	bool IsLearningDisabled(const json& settings) {
		return settings.value("learningdisabled", false);
	}

	bool IsLearningEnabled(const json& settings) {
		return !IsLearningDisabled(settings);
	}

	bool IsTalkative(const json& settings) {
		return settings.value("talkative", false);
	}

	std::vector<uint64_t> GetIgnoreList(const json& settings) {
		std::vector<uint64_t> ignores;
		if (settings.find("ignores") != settings.end()) {
			for (auto i = settings["ignores"].begin(); i != settings["ignores"].end(); ++i) {
				ignores.push_back(i->get<uint64_t>());
			}
		}
		return ignores;
	}
};




