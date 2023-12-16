#include <90s/orm/orm.hpp>

namespace db {
    using namespace s90;
    struct servers : public orm::with_orm {
        util::varstr<255> cookie;
        int timeLeft;
        int ratingUpdates;
        orm::datetime lastUpdated;
        bool dx10;
        util::varstr<255> pak;
        util::varstr<32000> players;
        util::varstr<255> mapName;
        bool behindProxy;
        int port;
        int gamespyPort;
        int peopleTime;
        util::varstr<255> localIp;
        util::varstr<255> game;
        int gameVersion;
        util::varstr<255> map;
        int uptime;
        util::varstr<255> description;
        bool voiceChat;
        int numPlayers;
        int activeTime;
        bool gamepadsOnly;
        bool isReal;
        util::varstr<255> ip;
        bool antiCheat;
        bool dedicated;
        util::varstr<255> source;
        double rating;
        bool ranked;
        int publicPort;
        util::varstr<255> password;
        util::varstr<255> name;
        int maxPlayers;
        util::varstr<255> mapDownloadLink;
        util::varstr<32000> gamespyPlayers;
        bool friendlyFire;

        orm::mapper get_orm() {
            return {
                { "cookie", cookie },
                { "time_left", timeLeft },
                { "rating_updates", ratingUpdates },
                { "last_updated", lastUpdated },
                { "dx10", dx10 },
                { "pak", pak },
                { "players", players },
                { "map_name", mapName },
                { "behind_proxy", behindProxy },
                { "port", port },
                { "gamespy_port", gamespyPort },
                { "people_time", peopleTime },
                { "local_ip", localIp },
                { "game", game },
                { "game_version", gameVersion },
                { "map", map },
                { "uptime", uptime },
                { "description", description },
                { "voice_chat", voiceChat },
                { "num_players", numPlayers },
                { "active_time", activeTime },
                { "gamepads_only", gamepadsOnly },
                { "is_real", isReal },
                { "ip", ip },
                { "anti_cheat", antiCheat },
                { "dedicated", dedicated },
                { "source", source },
                { "rating", rating },
                { "ranked", ranked },
                { "public_port", publicPort },
                { "password", password },
                { "name", name },
                { "max_players", maxPlayers },
                { "map_download_link", mapDownloadLink },
                { "gamespy_players", gamespyPlayers },
                { "friendly_fire", friendlyFire },
            };
        }
    };

    struct posts : public orm::with_orm {
        int author;
        int likeCount;
        int invisible;
        int thread;
        int createdAt;
        util::varstr<32000> text;
        int id;

        orm::mapper get_orm() {
            return {
                { "author", author },
                { "like_count", likeCount },
                { "invisible", invisible },
                { "thread", thread },
                { "date", createdAt },
                { "text", text },
                { "id", id },
            };
        }
    };

    struct categories : public orm::with_orm {
        std::string name;
        int rights;
        int id;

        orm::mapper get_orm() {
            return {
                { "name", name },
                { "rights", rights },
                { "id", id },
            };
        }
    };

    struct likes : public orm::with_orm {
        int userId;
        int postId;

        orm::mapper get_orm() {
            return {
                { "user_id", userId },
                { "post_id", postId },
            };
        }
    };

    struct statistics : public orm::with_orm {
        int port;
        int playerId;
        util::varstr<255> ip;
        util::varstr<255> name;
        int playedTime;
        int deaths;
        int kills;

        orm::mapper get_orm() {
            return {
                { "port", port },
                { "player_id", playerId },
                { "ip", ip },
                { "name", name },
                { "time", playedTime },
                { "deaths", deaths },
                { "kills", kills },
            };
        }
    };

    struct releases : public orm::with_orm {
        util::varstr<64> hash64;
        util::varstr<64> hash32;
        orm::datetime lastUpdated;
        util::varstr<64> commit;
        util::varstr<20> releaseType;

        orm::mapper get_orm() {
            return {
                { "hash_64", hash64 },
                { "hash_32", hash32 },
                { "updated_at", lastUpdated },
                { "commit_hash", commit },
                { "release_type", releaseType },
            };
        }
    };

    struct threads : public orm::with_orm {
        int author;
        int lastPostId;
        int id;
        int hidden;
        int lastPostTime;
        int createdAt;
        int encrypted;
        int subc;
        util::varstr<100> name;

        orm::mapper get_orm() {
            return {
                { "author", author },
                { "last_post_id", lastPostId },
                { "id", id },
                { "hidden", hidden },
                { "last_post_time", lastPostTime },
                { "date", createdAt },
                { "encrypted", encrypted },
                { "subc", subc },
                { "name", name },
            };
        }
    };

    struct staticIds : public orm::with_orm {
        orm::datetime createdAt;
        double tz;
        util::varstr<10> clientVersion;
        util::varstr<12> locale;
        int id;
        util::varstr<20> ip;
        util::varstr<96> hwid;
        orm::datetime lastLaunch;
        int launches;

        orm::mapper get_orm() {
            return {
                { "created", createdAt },
                { "tz", tz },
                { "ver", clientVersion },
                { "lng", locale },
                { "id", id },
                { "ip", ip },
                { "hwid", hwid },
                { "last_launch", lastLaunch },
                { "launches", launches },
            };
        }
    };

    struct maps : public orm::with_orm {
        bool autoGenerated;
        util::varstr<255> version;
        util::varstr<255> creatorName;
        util::varstr<255> mapName;
        util::varstr<255> url;

        orm::mapper get_orm() {
            return {
                { "auto_generated", autoGenerated },
                { "version", version },
                { "creator_name", creatorName },
                { "map_name", mapName },
                { "url", url },
            };
        }
    };

    struct subcategories : public orm::with_orm {
        std::string name;
        int main;
        int rights;
        int id;

        orm::mapper get_orm() {
            return {
                { "name", name },
                { "main", main },
                { "rights", rights },
                { "id", id },
            };
        }
    };

    struct users : public orm::with_orm {
        int rights;
        util::varstr<100> password;
        util::varstr<256> picture;
        util::varstr<128> email;
        int id;
        orm::datetime lastAttempt;
        util::varstr<1000> motto;
        std::string uid;
        int lastSeen;
        int loginAttempts;
        int createdAt;
        util::varstr<26> display;
        util::varstr<100> nick;

        orm::mapper get_orm() {
            return {
                { "rights", rights },
                { "password", password },
                { "photo", picture },
                { "email", email },
                { "id", id },
                { "last_attempt", lastAttempt },
                { "status", motto },
                { "uID", uid },
                { "online", lastSeen },
                { "login_attempts", loginAttempts },
                { "date", createdAt },
                { "display", display },
                { "nick", nick },
            };
        }
    };

}
