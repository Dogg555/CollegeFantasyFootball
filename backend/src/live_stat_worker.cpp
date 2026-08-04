#include "live_stat_worker.h"

#include "app_config.h"
#include "live_scores.h"
#include "live_stat_orchestration.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <ctime>
#include <iostream>
#include <optional>
#include <pqxx/pqxx>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace cff::live_stats {
namespace {

constexpr const char *kProvider = "cfbd";
constexpr const char *kSource = "scoreboard_schedule_cache";

struct ClaimResult {
    bool accepted{false};
    std::string code;
    std::string runId;
    std::string error;
};

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string joinErrors(const std::vector<std::string> &errors) {
    std::ostringstream output;
    for (std::size_t index = 0; index < errors.size(); ++index) {
        if (index > 0) output << " | ";
        output << errors[index];
    }
    auto text = output.str();
    constexpr std::size_t kMaximum = 4000;
    if (text.size() > kMaximum) text.resize(kMaximum);
    return text;
}

std::string compactJson(const Json::Value &value) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, value);
}

std::string generatedToken() {
    static std::atomic<unsigned long long> sequence{0};
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return std::to_string(millis) + "-" + std::to_string(++sequence);
}

std::string generatedRunId(int season, int week) {
    return std::string{kProvider} + "-" + std::to_string(season) + "-" +
           std::to_string(week) + "-" + generatedToken();
}

int currentSeason() {
    const auto now = std::chrono::system_clock::now();
    const auto raw = std::chrono::system_clock::to_time_t(now);
    const auto utc = *std::gmtime(&raw);
    const int year = 1900 + utc.tm_year;
    return utc.tm_mon == 0 ? year - 1 : year;
}

bool validRequest(const WorkerRequest &request, std::string &error) {
    if (request.season < 2000 || request.season > 2100) {
        error = "season must be between 2000 and 2100";
        return false;
    }
    if (request.week < 0 || request.week > 20) {
        error = "week must be between 0 and 20";
        return false;
    }
    if (request.runKey.size() > 160) {
        error = "runKey must be 160 characters or fewer";
        return false;
    }
    return true;
}

bool retryable(const std::vector<std::string> &errors) {
    if (errors.empty()) return false;
    for (const auto &error : errors) {
        const auto normalized = lower(error);
        if (normalized.find("is required") != std::string::npos ||
            normalized.find("authentication failed") != std::string::npos ||
            normalized.find("must be a four-digit") != std::string::npos) {
            return false;
        }
    }
    return true;
}

void insertOperatorEvent(const std::string &dbUrl,
                         const std::optional<std::string> &runId,
                         const std::string &severity,
                         const std::string &eventType,
                         const std::string &message,
                         const Json::Value &metadata = Json::Value{Json::objectValue}) {
    try {
        pqxx::connection connection{dbUrl};
        pqxx::work transaction{connection};
        if (runId && !runId->empty()) {
            transaction.exec_params(
                "INSERT INTO ingest_operator_events "
                "(run_id,severity,event_type,message,metadata) "
                "VALUES($1,$2,$3,$4,$5::jsonb)",
                *runId, severity, eventType, message, compactJson(metadata));
        } else {
            transaction.exec_params(
                "INSERT INTO ingest_operator_events "
                "(run_id,severity,event_type,message,metadata) "
                "VALUES(NULL,$1,$2,$3,$4::jsonb)",
                severity, eventType, message, compactJson(metadata));
        }
        transaction.commit();
    } catch (const std::exception &error) {
        std::cerr << "[live-stats] unable to persist operator event: "
                  << error.what() << std::endl;
    }
}

ClaimResult claimRun(const std::string &dbUrl, const WorkerRequest &request) {
    ClaimResult result;
    try {
        pqxx::connection connection{dbUrl};
        pqxx::work transaction{connection};

        const auto activeRows = transaction.exec_params(
            "SELECT id FROM stat_ingest_runs "
            "WHERE provider=$1 AND season=$2 AND week=$3 "
            "AND status IN ('queued','running') "
            "ORDER BY created_at DESC LIMIT 1",
            kProvider, request.season, request.week);
        const bool active = !activeRows.empty();

        const auto dedupeMinutes = static_cast<int>(
            cff::config::readSizeEnv("CFF_LIVE_STAT_DEDUPE_MINUTES", 2, 60));
        const auto recentRows = transaction.exec_params(
            "SELECT id FROM stat_ingest_runs "
            "WHERE provider=$1 AND season=$2 AND week=$3 "
            "AND status IN ('partial','succeeded','failed','skipped') "
            "AND completed_at >= NOW() - ($4::text || ' minutes')::interval "
            "ORDER BY completed_at DESC LIMIT 1",
            kProvider, request.season, request.week, dedupeMinutes);
        const bool recent = !recentRows.empty();

        const auto decision = mayStartRun(active, recent, request.force);
        if (!decision.start) {
            result.code = decision.code;
            if (active) result.runId = activeRows[0][0].c_str();
            if (!active && recent) result.runId = recentRows[0][0].c_str();
            transaction.commit();
            return result;
        }

        result.runId = generatedRunId(request.season, request.week);
        std::string runKey = request.runKey.empty() ? result.runId : request.runKey;
        const auto inserted = transaction.exec_params(
            "INSERT INTO stat_ingest_runs "
            "(id,provider,season,week,run_key,status,force_requested,started_at) "
            "VALUES($1,$2,$3,$4,$5,'running',$6,NOW()) "
            "ON CONFLICT DO NOTHING RETURNING id",
            result.runId, kProvider, request.season, request.week, runKey, request.force);
        if (inserted.empty()) {
            const auto existing = transaction.exec_params(
                "SELECT id FROM stat_ingest_runs "
                "WHERE provider=$1 AND season=$2 AND week=$3 "
                "AND status IN ('queued','running') "
                "ORDER BY created_at DESC LIMIT 1",
                kProvider, request.season, request.week);
            result.accepted = false;
            result.code = "ingest_already_running";
            if (!existing.empty()) result.runId = existing[0][0].c_str();
            transaction.commit();
            return result;
        }

        result.accepted = true;
        result.code = decision.code;
        transaction.commit();
        return result;
    } catch (const std::exception &error) {
        result.code = "claim_failed";
        result.error = error.what();
        return result;
    }
}

void persistCompletedRun(const std::string &dbUrl,
                         const WorkerRequest &request,
                         const std::string &runId,
                         const LiveScoreIngestResult &ingest,
                         RunStatus status,
                         int attempts) {
    const bool succeeded = status == RunStatus::succeeded;
    const auto errorSummary = joinErrors(ingest.errors);
    try {
        pqxx::connection connection{dbUrl};
        pqxx::work transaction{connection};
        transaction.exec_params(
            "INSERT INTO stat_ingest_source_results "
            "(run_id,source,status,rows_received,rows_changed,error_message,observed_at,completed_at) "
            "VALUES($1,$2,$3,$4,0,$5,NOW(),NOW()) "
            "ON CONFLICT(run_id,source) DO UPDATE SET "
            "status=EXCLUDED.status,rows_received=EXCLUDED.rows_received,"
            "rows_changed=EXCLUDED.rows_changed,error_message=EXCLUDED.error_message,"
            "observed_at=EXCLUDED.observed_at,completed_at=NOW()",
            runId,
            kSource,
            succeeded ? "succeeded" : "failed",
            static_cast<int>(ingest.games),
            errorSummary);

        transaction.exec_params(
            "UPDATE stat_ingest_runs SET status=$2,rows_changed=0,error_summary=$3,"
            "completed_at=NOW() WHERE id=$1",
            runId, toString(status), errorSummary);

        if (succeeded) {
            transaction.exec_params(
                "INSERT INTO stat_source_freshness "
                "(provider,source,season,week,state,last_attempt_at,last_success_at,"
                "last_complete_run_id,consecutive_failures,updated_at) "
                "VALUES($1,$2,$3,$4,'fresh',NOW(),NOW(),$5,0,NOW()) "
                "ON CONFLICT(provider,source,season,week) DO UPDATE SET "
                "state='fresh',last_attempt_at=NOW(),last_success_at=NOW(),"
                "last_complete_run_id=EXCLUDED.last_complete_run_id,"
                "consecutive_failures=0,updated_at=NOW()",
                kProvider, kSource, request.season, request.week, runId);
        } else {
            transaction.exec_params(
                "INSERT INTO stat_source_freshness "
                "(provider,source,season,week,state,last_attempt_at,consecutive_failures,updated_at) "
                "VALUES($1,$2,$3,$4,'unavailable',NOW(),1,NOW()) "
                "ON CONFLICT(provider,source,season,week) DO UPDATE SET "
                "state=CASE WHEN stat_source_freshness.last_success_at IS NULL "
                "THEN 'unavailable' ELSE 'partial' END,"
                "last_attempt_at=NOW(),consecutive_failures="
                "stat_source_freshness.consecutive_failures+1,updated_at=NOW()",
                kProvider, kSource, request.season, request.week);
        }

        Json::Value metadata;
        metadata["attempts"] = attempts;
        metadata["games"] = static_cast<Json::UInt64>(ingest.games);
        metadata["liveGames"] = static_cast<Json::UInt64>(ingest.liveGames);
        metadata["apiCalls"] = static_cast<Json::UInt64>(ingest.apiCalls);
        metadata["scoringRefreshQueued"] = 0;
        transaction.exec_params(
            "INSERT INTO ingest_operator_events "
            "(run_id,severity,event_type,message,metadata) "
            "VALUES($1,$2,$3,$4,$5::jsonb)",
            runId,
            succeeded ? "info" : "error",
            succeeded ? "live_ingest_succeeded" : "live_ingest_failed",
            succeeded ? "CFBD live score cache refresh completed." :
                        "CFBD live score cache refresh failed.",
            compactJson(metadata));
        transaction.commit();
    } catch (const std::exception &error) {
        std::cerr << "[live-stats] unable to persist completed run: "
                  << error.what() << std::endl;
    }
}

Json::Value unavailableStatus(const std::string &message) {
    Json::Value payload;
    payload["status"] = "unavailable";
    payload["error"] = message;
    payload["runs"] = Json::Value{Json::arrayValue};
    payload["freshness"] = Json::Value{Json::arrayValue};
    payload["events"] = Json::Value{Json::arrayValue};
    payload["queue"] = Json::Value{Json::objectValue};
    return payload;
}

} // namespace

int configuredLiveStatSeason() {
    const auto configured = cff::config::readEnv("CFBD_SEASON");
    if (configured) {
        try {
            const int parsed = std::stoi(*configured);
            if (parsed >= 2000 && parsed <= 2100) return parsed;
        } catch (...) {
        }
    }
    return currentSeason();
}

int configuredLiveStatWeek() {
    const auto configured = cff::config::readPositiveIntEnv("CFF_CURRENT_WEEK");
    if (!configured) return 0;
    return std::clamp(*configured, 1, 20);
}

Json::Value runCfbdLiveStatWorker(const WorkerRequest &provided) {
    WorkerRequest request = provided;
    if (request.season == 0) request.season = configuredLiveStatSeason();
    if (request.week < 0) request.week = configuredLiveStatWeek();

    Json::Value payload;
    payload["provider"] = kProvider;
    payload["season"] = request.season;
    payload["week"] = request.week;
    payload["force"] = request.force;
    payload["accepted"] = false;
    payload["scoringRefreshQueued"] = 0;
    payload["scoringRefreshReady"] = false;

    std::string validationError;
    if (!validRequest(request, validationError)) {
        payload["status"] = "invalid";
        payload["code"] = "invalid_request";
        payload["error"] = validationError;
        return payload;
    }

    const auto dbUrl = cff::config::readEnv("DB_URL");
    if (!dbUrl) {
        payload["status"] = "unavailable";
        payload["code"] = "database_not_configured";
        payload["error"] = "DB_URL is required for durable live stat ingestion.";
        return payload;
    }

    const auto claim = claimRun(*dbUrl, request);
    payload["accepted"] = claim.accepted;
    payload["code"] = claim.code;
    payload["runId"] = claim.runId;
    if (!claim.error.empty()) payload["error"] = claim.error;
    if (!claim.accepted) {
        payload["status"] = claim.code == "duplicate_ingest" ? "duplicate" : "skipped";
        Json::Value metadata;
        metadata["season"] = request.season;
        metadata["week"] = request.week;
        metadata["code"] = claim.code;
        insertOperatorEvent(
            *dbUrl,
            claim.runId.empty() ? std::nullopt : std::optional<std::string>{claim.runId},
            claim.code == "claim_failed" ? "error" : "info",
            claim.code,
            claim.code == "ingest_already_running"
                ? "A matching live stat refresh is already running."
                : "A matching recent live stat refresh was not repeated.",
            metadata);
        return payload;
    }

    const int maxAttempts = static_cast<int>(
        cff::config::readSizeEnv("CFF_LIVE_STAT_MAX_ATTEMPTS", 3, 5));
    const int baseBackoffMs = static_cast<int>(
        cff::config::readSizeEnv("CFF_LIVE_STAT_RETRY_BASE_MS", 750, 10000));

    LiveScoreIngestResult ingest;
    int attempts = 0;
    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
        attempts = attempt;
        ingest = cff::runLiveScoreIngestOnce();
        if (ingest.errors.empty()) break;
        if (attempt >= maxAttempts || !retryable(ingest.errors)) break;

        const int delay = baseBackoffMs * (1 << (attempt - 1));
        Json::Value metadata;
        metadata["attempt"] = attempt;
        metadata["nextAttempt"] = attempt + 1;
        metadata["delayMs"] = delay;
        metadata["error"] = joinErrors(ingest.errors);
        insertOperatorEvent(
            *dbUrl,
            claim.runId,
            "warning",
            "live_ingest_retry",
            "CFBD live score cache refresh will be retried.",
            metadata);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }

    SourceResult source;
    source.source = kSource;
    source.attempted = true;
    source.succeeded = ingest.errors.empty();
    source.rows = ingest.games;
    source.error = joinErrors(ingest.errors);
    source.observedAt = std::chrono::system_clock::now();
    const auto finalStatus = aggregateStatus({source});
    persistCompletedRun(*dbUrl, request, claim.runId, ingest, finalStatus, attempts);

    payload["status"] = toString(finalStatus);
    payload["attempts"] = attempts;
    payload["apiCalls"] = static_cast<Json::UInt64>(ingest.apiCalls);
    payload["games"] = static_cast<Json::UInt64>(ingest.games);
    payload["liveGames"] = static_cast<Json::UInt64>(ingest.liveGames);
    payload["scheduleGames"] = static_cast<Json::UInt64>(ingest.scheduleGames);
    payload["scheduleRefreshed"] = ingest.scheduleRefreshed;
    if (!ingest.errors.empty()) {
        Json::Value errors{Json::arrayValue};
        for (const auto &error : ingest.errors) errors.append(error);
        payload["errors"] = errors;
    }
    payload["note"] =
        "The current adapter refreshes scoreboard and schedule cache data. "
        "Fantasy scoring refresh remains disabled until the CFBD player-stat adapter is available.";
    return payload;
}

Json::Value liveStatOperatorStatus(int season, int week) {
    const auto dbUrl = cff::config::readEnv("DB_URL");
    if (!dbUrl) return unavailableStatus("DB_URL is not configured.");

    Json::Value payload;
    payload["status"] = "ok";
    payload["provider"] = kProvider;
    payload["filter"]["season"] = season;
    payload["filter"]["week"] = week;
    payload["capabilities"]["scoreboardScheduleAdapter"] = true;
    payload["capabilities"]["playerStatsAdapter"] = false;
    payload["capabilities"]["scoringRefreshWorker"] = false;
    payload["capabilities"]["durableRunClaiming"] = true;
    payload["capabilities"]["boundedRetries"] = true;
    payload["cache"] = cff::liveScoreIngestStatus();
    payload["runs"] = Json::Value{Json::arrayValue};
    payload["freshness"] = Json::Value{Json::arrayValue};
    payload["events"] = Json::Value{Json::arrayValue};
    payload["queue"] = Json::Value{Json::objectValue};

    try {
        pqxx::connection connection{*dbUrl};
        pqxx::read_transaction transaction{connection};

        const auto runs = transaction.exec_params(
            "SELECT id,season,week,run_key,status,force_requested,rows_changed,"
            "COALESCE(error_summary,''),"
            "COALESCE(to_char(started_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'),''),"
            "COALESCE(to_char(completed_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'),'') "
            "FROM stat_ingest_runs WHERE provider=$1 "
            "AND ($2=0 OR season=$2) AND ($3<0 OR week=$3) "
            "ORDER BY created_at DESC LIMIT 20",
            kProvider, season, week);
        for (const auto &row : runs) {
            Json::Value run;
            const std::string runId = row[0].c_str();
            run["id"] = runId;
            run["season"] = row[1].as<int>();
            run["week"] = row[2].as<int>();
            run["runKey"] = row[3].c_str();
            run["status"] = row[4].c_str();
            run["forceRequested"] = row[5].as<bool>();
            run["rowsChanged"] = row[6].as<int>();
            run["error"] = row[7].c_str();
            run["startedAt"] = row[8].c_str();
            run["completedAt"] = row[9].c_str();
            run["sources"] = Json::Value{Json::arrayValue};

            const auto sources = transaction.exec_params(
                "SELECT source,status,rows_received,rows_changed,COALESCE(error_message,''),"
                "COALESCE(to_char(completed_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'),'') "
                "FROM stat_ingest_source_results WHERE run_id=$1 ORDER BY source",
                runId);
            for (const auto &source : sources) {
                Json::Value item;
                item["source"] = source[0].c_str();
                item["status"] = source[1].c_str();
                item["rowsReceived"] = source[2].as<int>();
                item["rowsChanged"] = source[3].as<int>();
                item["error"] = source[4].c_str();
                item["completedAt"] = source[5].c_str();
                run["sources"].append(item);
            }
            payload["runs"].append(run);
        }

        const auto freshness = transaction.exec_params(
            "SELECT source,season,week,state,consecutive_failures,"
            "COALESCE(to_char(last_attempt_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'),''),"
            "COALESCE(to_char(last_success_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'),'') "
            "FROM stat_source_freshness WHERE provider=$1 "
            "AND ($2=0 OR season=$2) AND ($3<0 OR week=$3) "
            "ORDER BY season DESC,week DESC,source",
            kProvider, season, week);
        for (const auto &row : freshness) {
            Json::Value item;
            item["source"] = row[0].c_str();
            item["season"] = row[1].as<int>();
            item["week"] = row[2].as<int>();
            item["state"] = row[3].c_str();
            item["consecutiveFailures"] = row[4].as<int>();
            item["lastAttemptAt"] = row[5].c_str();
            item["lastSuccessAt"] = row[6].c_str();
            payload["freshness"].append(item);
        }

        const auto queue = transaction.exec(
            "SELECT status,COUNT(*) FROM scoring_refresh_queue GROUP BY status");
        int total = 0;
        for (const auto &row : queue) {
            const int count = row[1].as<int>();
            payload["queue"][row[0].c_str()] = count;
            total += count;
        }
        payload["queue"]["total"] = total;

        const auto events = transaction.exec(
            "SELECT COALESCE(run_id,''),severity,event_type,message,metadata::text,"
            "to_char(created_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') "
            "FROM ingest_operator_events ORDER BY created_at DESC LIMIT 20");
        for (const auto &row : events) {
            Json::Value item;
            item["runId"] = row[0].c_str();
            item["severity"] = row[1].c_str();
            item["eventType"] = row[2].c_str();
            item["message"] = row[3].c_str();
            item["metadata"] = row[4].c_str();
            item["createdAt"] = row[5].c_str();
            payload["events"].append(item);
        }
        return payload;
    } catch (const std::exception &error) {
        return unavailableStatus(error.what());
    }
}

void configureLiveStatWorker() {
    const bool runOnStartup =
        cff::config::envFlagEnabled("CFF_LIVE_STAT_ON_STARTUP");
    const auto intervalMinutes =
        cff::config::readPositiveIntEnv("CFF_LIVE_STAT_INTERVAL_MINUTES");
    if (!runOnStartup && !intervalMinutes) return;

    std::thread([runOnStartup, intervalMinutes]() {
        const auto run = []() {
            WorkerRequest request;
            request.season = configuredLiveStatSeason();
            request.week = configuredLiveStatWeek();
            const auto result = runCfbdLiveStatWorker(request);
            std::cout << "[live-stats] worker status="
                      << result.get("status", "unknown").asString()
                      << " code=" << result.get("code", "").asString()
                      << std::endl;
        };

        if (runOnStartup) run();
        if (!intervalMinutes) return;
        std::cout << "[live-stats] scheduled worker enabled every "
                  << *intervalMinutes << " minute(s)." << std::endl;
        while (true) {
            std::this_thread::sleep_for(std::chrono::minutes(*intervalMinutes));
            run();
        }
    }).detach();
}

} // namespace cff::live_stats
