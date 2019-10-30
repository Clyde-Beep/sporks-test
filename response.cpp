#include <string>
#include <mutex>
#include <string>
#include <sstream>
#include "bot.h"
#include "regex.h"
#include "queue.h"
#include "config.h"
#include "stringops.h"
#include "status.h"

void Bot::OutputThread() {
	PCRE statsreply("Since (.+?), there have been (\\d+) modifications and (\\d+) questions. I have been alive for (.+?), I currently know (\\d+)");
	PCRE url_sanitise("^https?://", true);
	while (!this->terminate) {
		try {
			std::queue<QueueItem> done;
			do {
				std::lock_guard<std::mutex> output_lock(this->output_mutex);
				while (!outputs.empty()) {
					done.push(outputs.front());
					outputs.pop();
				}
			} while (false);
			while (!done.empty()) {
				rapidjson::Document channel_settings;
				do {
					std::lock_guard<std::mutex> hash_lock(this->channel_hash_mutex);
					//channel = this->channelList.find(done.front().channelID)->second;
					channel_settings = getSettings(this, done.front().channelID, done.front().serverID);
				} while (false);
				if (done.front().mentioned || settings::IsTalkative(channel_settings)) {
					try {
						std::vector<std::string> m;
						if (statsreply.Match(done.front().message, m)) {
							/* Handle status reply */
							ShowStatus(this, m, done.front().channelID);
						} else {
							/* Anything else */
							std::string message = trim(done.front().message);

							/* Translate IRC actions */
							if (message.substr(0, 8) == "\001ACTION ") {
								message = "*" + message.substr(8, message.length() - 9) + "*";
							}

							/* Sanitise by putting <> around all but the first url in the message to stop embed spam */
							size_t urls_matched = 0;
							std::stringstream ss(message);
							std::string word;
							while (ss) {
								ss >> word;
								if (url_sanitise.Match(word)) {
									if (urls_matched > 0) {
										message = ReplaceString(message, word, "<" + word + ">");
									}
									urls_matched++;
								}
							}
							//this->sendMessage(done.front().channelID, message);
							//FIXME
						}
					}
					catch (std::exception e) { /* FIXME */
						std::cout << "Can't send message to channel id " << done.front().channelID << " (talkative=" << settings::IsTalkative(channel_settings) << ",mentioned=" << done.front().mentioned << ") message was: '" << done.front().message << "'" << std::endl;
					}
				}
				done.pop();
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}
		catch (std::exception e) {
			// FIXME
			std::cout << "Response Oof!" << std::endl;
		}
	}
}

