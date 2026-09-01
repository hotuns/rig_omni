#include "custom_control_client.h"

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/semphr.h>
#include <http.h>
#include <web_socket.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <string_view>
#include <sys/time.h>

#include "application.h"
#include "board.h"
#include "mcp_server.h"
#include "settings.h"
#include "system_info.h"
#include "ssid_manager.h"
#include "wifi_manager.h"

#define TAG "CustomControl"

#ifndef CONFIG_CUSTOM_CONTROL_SERVER_URL
#define CONFIG_CUSTOM_CONTROL_SERVER_URL ""
#endif
#ifndef CONFIG_CUSTOM_CONTROL_TOKEN
#define CONFIG_CUSTOM_CONTROL_TOKEN ""
#endif

namespace {
constexpr int kReconnectDelayUs = 5 * 1000 * 1000;
constexpr int kStatusIntervalUs = 30 * 1000 * 1000;

std::string JsonString(const cJSON* value) {
    if (!value) {
        return "{}";
    }
    char* raw = cJSON_PrintUnformatted(value);
    std::string result = raw ? raw : "{}";
    cJSON_free(raw);
    return result;
}

std::string GetString(const cJSON* object, const char* key) {
    auto value = cJSON_GetObjectItem(object, key);
    return cJSON_IsString(value) ? value->valuestring : "";
}

std::string LoadChunked(const char* prefix, const std::string& fallback) {
    Settings settings("custom_ctl", false);
    int count = settings.GetInt(std::string(prefix) + "_count", 0);
    if (count <= 0) return fallback;
    std::string value;
    for (int i = 0; i < count; ++i) {
        value += settings.GetString(std::string(prefix) + "_" + std::to_string(i));
    }
    return value.empty() ? fallback : value;
}

void SaveChunked(const char* prefix, const std::string& value) {
    constexpr size_t kChunkSize = 3000;
    Settings settings("custom_ctl", true);
    int count = static_cast<int>((value.size() + kChunkSize - 1) / kChunkSize);
    settings.SetInt(std::string(prefix) + "_count", count);
    for (int i = 0; i < count; ++i) {
        settings.SetString(std::string(prefix) + "_" + std::to_string(i),
                           value.substr(i * kChunkSize, kChunkSize));
    }
}
}

CustomControlClient& CustomControlClient::GetInstance() {
    static CustomControlClient instance;
    return instance;
}

CustomControlClient::CustomControlClient() {
    job_queue_ = xQueueCreate(8, sizeof(Job*));

    esp_timer_create_args_t reconnect_args = {
        .callback = [](void* arg) {
            auto self = static_cast<CustomControlClient*>(arg);
            xTaskCreate([](void* task_arg) {
                static_cast<CustomControlClient*>(task_arg)->Connect();
                vTaskDelete(nullptr);
            }, "custom_reconnect", 6144, self, 2, nullptr);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "custom_reconnect",
    };
    esp_timer_create(&reconnect_args, &reconnect_timer_);

    esp_timer_create_args_t status_args = {
        .callback = [](void* arg) {
            auto self = static_cast<CustomControlClient*>(arg);
            self->SendStatus();
            self->CheckReminders();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "custom_status",
    };
    esp_timer_create(&status_args, &status_timer_);
}

CustomControlClient::~CustomControlClient() {
    Stop();
    if (job_queue_) {
        vQueueDelete(job_queue_);
    }
}

void CustomControlClient::LoadSettings() {
    Settings settings("custom_ctl", false);
    server_url_ = settings.GetString("url", CONFIG_CUSTOM_CONTROL_SERVER_URL);
    token_ = settings.GetString("token", CONFIG_CUSTOM_CONTROL_TOKEN);
    workflows_json_ = LoadChunked("wf", "{\"workflows\":[]}");
    reminders_json_ = LoadChunked("rm", "{\"reminders\":[]}");
    device_id_ = SystemInfo::GetMacAddress();
}

void CustomControlClient::Start() {
    if (started_) {
        return;
    }
    started_ = true;
    LoadSettings();
    if (!worker_task_) {
        xTaskCreate([](void* arg) {
            static_cast<CustomControlClient*>(arg)->WorkerLoop();
        }, "custom_worker", 8192, this, 3, &worker_task_);
    }
    esp_timer_start_periodic(status_timer_, kStatusIntervalUs);
    if (!server_url_.empty()) {
        Connect();
    } else {
        ESP_LOGI(TAG, "Custom server URL is empty; offline workflows remain enabled");
    }
}

void CustomControlClient::Stop() {
    started_ = false;
    if (reconnect_timer_) esp_timer_stop(reconnect_timer_);
    if (status_timer_) esp_timer_stop(status_timer_);
    std::lock_guard<std::mutex> lock(socket_mutex_);
    websocket_.reset();
}

bool CustomControlClient::IsConnected() const {
    std::lock_guard<std::mutex> lock(socket_mutex_);
    return websocket_ && websocket_->IsConnected();
}

void CustomControlClient::Connect() {
    if (!started_ || server_url_.empty() || IsConnected()) {
        return;
    }

    auto socket = Board::GetInstance().GetNetwork()->CreateWebSocket(1);
    if (!socket) {
        ScheduleReconnect();
        return;
    }
    if (!token_.empty()) {
        std::string authorization = "Bearer " + token_;
        socket->SetHeader("Authorization", authorization.c_str());
    }
    socket->SetHeader("X-Device-Id", device_id_.c_str());
    socket->OnData([this](const char* data, size_t len, bool binary) {
        if (!binary) HandleMessage(data, len);
    });
    socket->OnDisconnected([this]() {
        ESP_LOGW(TAG, "Custom server disconnected");
        ScheduleReconnect();
    });

    if (!socket->Connect(server_url_.c_str())) {
        ESP_LOGW(TAG, "Failed to connect custom server: %s", server_url_.c_str());
        ScheduleReconnect();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        websocket_ = std::move(socket);
    }
    SendHello();
    SendStatus();
}

void CustomControlClient::ScheduleReconnect() {
    if (!started_ || esp_timer_is_active(reconnect_timer_)) return;
    esp_timer_start_once(reconnect_timer_, kReconnectDelayUs);
}

void CustomControlClient::HandleMessage(const char* data, size_t len) {
    auto root = cJSON_ParseWithLength(data, len);
    if (!root) return;
    std::string type = GetString(root, "type");
    if (type == "command.execute") {
        HandleCommand(root);
    } else if (type == "workflow.execute") {
        HandleWorkflow(root);
    } else if (type == "workflow.sync") {
        SyncPayload(root, "workflows", workflows_json_);
    } else if (type == "reminder.sync") {
        SyncPayload(root, "reminders", reminders_json_);
    } else if (type == "config.update") {
        UpdateConfiguration(root);
    } else if (type == "wifi.list") {
        HandleWifiList(root);
    } else if (type == "wifi.update") {
        HandleWifiUpdate(root);
    } else if (type == "wifi.forget") {
        HandleWifiForget(root);
    } else if (type == "heartbeat") {
        auto payload = cJSON_GetObjectItem(root, "payload");
        auto server_time = cJSON_GetObjectItem(payload, "server_time");
        if (cJSON_IsNumber(server_time) && server_time->valuedouble > 1700000000) {
            timeval tv = {.tv_sec = static_cast<time_t>(server_time->valuedouble), .tv_usec = 0};
            settimeofday(&tv, nullptr);
        }
        SendEnvelope("heartbeat", GetString(root, "id"), "{\"ok\":true}");
    }
    cJSON_Delete(root);
}

void CustomControlClient::HandleWifiList(const cJSON* root) {
    auto records = WifiManager::GetInstance().ScanAvailable();
    auto payload = cJSON_CreateObject();
    cJSON_AddBoolToObject(payload, "success", true);
    auto networks = cJSON_AddArrayToObject(payload, "networks");
    const auto& saved = SsidManager::GetInstance().GetSsidList();
    for (const auto& record : records) {
        auto item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", record.ssid.c_str());
        cJSON_AddNumberToObject(item, "rssi", record.rssi);
        cJSON_AddBoolToObject(item, "secure", record.authmode != WIFI_AUTH_OPEN);
        bool is_saved = std::any_of(saved.begin(), saved.end(), [&record](const SsidItem& value) { return value.ssid == record.ssid; });
        cJSON_AddBoolToObject(item, "saved", is_saved);
        cJSON_AddItemToArray(networks, item);
    }
    SendEnvelope("wifi.list.result", GetString(root, "id"), JsonString(payload));
    cJSON_Delete(payload);
}

void CustomControlClient::HandleWifiUpdate(const cJSON* root) {
    auto payload = cJSON_GetObjectItem(root, "payload");
    auto ssid = GetString(payload, "ssid");
    auto password = GetString(payload, "password");
    if (ssid.empty()) {
        SendEnvelope("wifi.update.result", GetString(root, "id"), "{\"success\":false,\"error\":\"SSID is required\"}");
        return;
    }
    auto& manager = SsidManager::GetInstance();
    manager.AddSsid(ssid, password);
    const auto& values = manager.GetSsidList();
    auto found = std::find_if(values.begin(), values.end(), [&ssid](const SsidItem& item) { return item.ssid == ssid; });
    if (found != values.end()) manager.SetDefaultSsid(static_cast<int>(std::distance(values.begin(), found)));
    SendEnvelope("wifi.update.result", GetString(root, "id"), "{\"success\":true,\"rebooting\":true}");
    vTaskDelay(pdMS_TO_TICKS(350));
    Application::GetInstance().Reboot();
}

void CustomControlClient::HandleWifiForget(const cJSON* root) {
    auto payload = cJSON_GetObjectItem(root, "payload");
    auto ssid = GetString(payload, "ssid");
    auto& manager = SsidManager::GetInstance();
    const auto& values = manager.GetSsidList();
    auto found = std::find_if(values.begin(), values.end(), [&ssid](const SsidItem& item) { return item.ssid == ssid; });
    if (found == values.end()) {
        SendEnvelope("wifi.forget.result", GetString(root, "id"), "{\"success\":false,\"error\":\"network not saved\"}");
        return;
    }
    manager.RemoveSsid(static_cast<int>(std::distance(values.begin(), found)));
    SendEnvelope("wifi.forget.result", GetString(root, "id"), "{\"success\":true}");
}

void CustomControlClient::HandleCommand(const cJSON* root) {
    auto payload = cJSON_GetObjectItem(root, "payload");
    auto workflow = cJSON_CreateObject();
    auto steps = cJSON_CreateArray();
    auto step = cJSON_CreateObject();
    cJSON_AddStringToObject(step, "type", "tool");
    cJSON_AddStringToObject(step, "name", GetString(payload, "name").c_str());
    auto args = cJSON_GetObjectItem(payload, "arguments");
    cJSON_AddItemToObject(step, "arguments", args ? cJSON_Duplicate(args, true) : cJSON_CreateObject());
    cJSON_AddItemToArray(steps, step);
    cJSON_AddItemToObject(workflow, "steps", steps);
    auto job = new Job{GetString(root, "id"), "command.result", JsonString(workflow)};
    cJSON_Delete(workflow);
    EnqueueJob(job);
}

void CustomControlClient::HandleWorkflow(const cJSON* root) {
    auto payload = cJSON_GetObjectItem(root, "payload");
    std::string workflow_json;
    auto workflow = cJSON_GetObjectItem(payload, "workflow");
    if (cJSON_IsObject(workflow)) {
        workflow_json = JsonString(workflow);
    } else {
        workflow_json = FindWorkflow(GetString(payload, "workflow_id"));
    }
    auto job = new Job{GetString(root, "id"), "workflow.result", workflow_json};
    EnqueueJob(job);
}

void CustomControlClient::SyncPayload(const cJSON* root, const char* key, std::string& target) {
    auto payload = cJSON_GetObjectItem(root, "payload");
    if (!cJSON_IsObject(payload)) return;
    target = JsonString(payload);
    SaveChunked(std::string(key) == "workflows" ? "wf" : "rm", target);
    SendEnvelope(std::string(key) == "workflows" ? "workflow.result" : "reminder.result",
                 GetString(root, "id"), "{\"success\":true}");
}

void CustomControlClient::UpdateConfiguration(const cJSON* root) {
    auto payload = cJSON_GetObjectItem(root, "payload");
    auto url = GetString(payload, "url");
    auto token = GetString(payload, "token");
    Settings settings("custom_ctl", true);
    if (!url.empty()) settings.SetString("url", url);
    if (!token.empty()) settings.SetString("token", token);
    auto wake_word = GetString(payload, "wake_word");
    auto wake_display = GetString(payload, "wake_display");
    auto wake_threshold = cJSON_GetObjectItem(payload, "wake_threshold");
    bool reboot = !wake_word.empty();
    if (!wake_word.empty()) settings.SetString("wake_word", wake_word);
    if (!wake_display.empty()) settings.SetString("wake_display", wake_display);
    if (cJSON_IsNumber(wake_threshold)) settings.SetInt("wake_threshold", wake_threshold->valueint);
    SendEnvelope("config.result", GetString(root, "id"), reboot ? "{\"success\":true,\"rebooting\":true}" : "{\"success\":true,\"reboot_required\":true}");
    if (reboot) Application::GetInstance().Reboot();
}

void CustomControlClient::EnqueueJob(Job* job) {
    if (!job || job->workflow_json.empty() || xQueueSend(job_queue_, &job, 0) != pdTRUE) {
        if (job) {
            SendEnvelope(job->response_type, job->request_id, "{\"success\":false,\"error\":\"job queue full or workflow missing\"}");
            delete job;
        }
    }
}

void CustomControlClient::WorkerLoop() {
    while (true) {
        Job* job = nullptr;
        if (xQueueReceive(job_queue_, &job, portMAX_DELAY) != pdTRUE || !job) continue;
        Application::GetInstance().InterruptForCustomControl();
        vTaskDelay(pdMS_TO_TICKS(150));
        std::string result;
        bool success = ExecuteWorkflow(job->workflow_json, result);
        cJSON* payload = cJSON_CreateObject();
        cJSON_AddBoolToObject(payload, "success", success);
        cJSON_AddStringToObject(payload, success ? "result" : "error", result.c_str());
        SendEnvelope(job->response_type, job->request_id, JsonString(payload));
        cJSON_Delete(payload);
        delete job;
    }
}

bool CustomControlClient::ExecuteWorkflow(const std::string& workflow_json, std::string& result) {
    auto workflow = cJSON_Parse(workflow_json.c_str());
    auto steps = cJSON_GetObjectItem(workflow, "steps");
    if (!workflow || !cJSON_IsArray(steps)) {
        if (workflow) cJSON_Delete(workflow);
        result = "invalid workflow";
        return false;
    }
    cJSON* step = nullptr;
    cJSON_ArrayForEach(step, steps) {
        std::string type = GetString(step, "type");
        if (type == "delay") {
            auto ms = cJSON_GetObjectItem(step, "ms");
            vTaskDelay(pdMS_TO_TICKS(cJSON_IsNumber(ms) ? ms->valueint : 0));
        } else if (type == "audio") {
            if (!PlayRemoteOgg(GetString(step, "url"), result)) {
                cJSON_Delete(workflow);
                return false;
            }
        } else if (type == "tool") {
            if (!ExecuteToolStep(step, result)) {
                cJSON_Delete(workflow);
                return false;
            }
        }
    }
    cJSON_Delete(workflow);
    result = "workflow completed";
    return true;
}

bool CustomControlClient::ExecuteToolStep(const cJSON* step, std::string& result) {
    auto done = xSemaphoreCreateBinary();
    bool success = false;
    auto arguments = cJSON_GetObjectItem(step, "arguments");
    McpServer::GetInstance().ExecuteTool(GetString(step, "name"), arguments,
        [&](const std::string& value) { success = true; result = value; xSemaphoreGive(done); },
        [&](const std::string& error) { result = error; xSemaphoreGive(done); });
    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
    return success;
}

bool CustomControlClient::PlayRemoteOgg(const std::string& url, std::string& error) {
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    if (!http || !http->Open("GET", url) || http->GetStatusCode() != 200) {
        error = "failed to download audio";
        return false;
    }
    size_t size = http->GetBodyLength();
    auto data = static_cast<char*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!data) {
        error = "failed to allocate audio buffer";
        return false;
    }
    size_t total = 0;
    while (total < size) {
        int read = http->Read(data + total, size - total);
        if (read <= 0) break;
        total += read;
    }
    http->Close();
    if (total != size) {
        heap_caps_free(data);
        error = "incomplete audio download";
        return false;
    }
    Application::GetInstance().PlaySound(std::string_view(data, size));
    Application::GetInstance().GetAudioService().WaitForPlaybackQueueEmpty();
    heap_caps_free(data);
    return true;
}

std::string CustomControlClient::FindWorkflow(const std::string& workflow_id) const {
    auto root = cJSON_Parse(workflows_json_.c_str());
    if (!root) return "";
    auto workflows = cJSON_GetObjectItem(root, "workflows");
    cJSON* workflow = nullptr;
    std::string result;
    cJSON_ArrayForEach(workflow, workflows) {
        if (GetString(workflow, "id") == workflow_id) {
            result = JsonString(workflow);
            break;
        }
    }
    cJSON_Delete(root);
    return result;
}

void CustomControlClient::CheckReminders() {
    auto root = cJSON_Parse(reminders_json_.c_str());
    if (!root) return;
    auto reminders = cJSON_GetObjectItem(root, "reminders");
    std::time_t now = std::time(nullptr);
    if (now < 1700000000) {
        cJSON_Delete(root);
        return;
    }
    cJSON* reminder = nullptr;
    cJSON_ArrayForEach(reminder, reminders) {
        auto enabled = cJSON_GetObjectItem(reminder, "enabled");
        auto interval = cJSON_GetObjectItem(reminder, "interval_minutes");
        std::string id = GetString(reminder, "id");
        if (!cJSON_IsTrue(enabled) || !cJSON_IsNumber(interval) || interval->valueint <= 0) continue;

        int offset = 0;
        auto offset_json = cJSON_GetObjectItem(reminder, "timezone_offset_minutes");
        if (cJSON_IsNumber(offset_json)) offset = offset_json->valueint;
        std::time_t local_time = now + offset * 60;
        std::tm local_tm = {};
        gmtime_r(&local_time, &local_tm);

        auto days = cJSON_GetObjectItem(reminder, "days");
        bool day_allowed = !cJSON_IsArray(days);
        cJSON* day = nullptr;
        cJSON_ArrayForEach(day, days) {
            if (cJSON_IsNumber(day) && day->valueint == local_tm.tm_wday) day_allowed = true;
        }
        if (!day_allowed) continue;

        int minute = local_tm.tm_hour * 60 + local_tm.tm_min;
        auto start = cJSON_GetObjectItem(reminder, "start_minute");
        auto end = cJSON_GetObjectItem(reminder, "end_minute");
        if (cJSON_IsNumber(start) && minute < start->valueint) continue;
        if (cJSON_IsNumber(end) && minute > end->valueint) continue;

        int64_t last = reminder_last_run_[id];
        if (last == 0) {
            reminder_last_run_[id] = now;
        } else if (now - last >= interval->valueint * 60) {
            reminder_last_run_[id] = now;
            auto job = new Job{id, "workflow.result", FindWorkflow(GetString(reminder, "workflow_id"))};
            EnqueueJob(job);
        }
    }
    cJSON_Delete(root);
}

void CustomControlClient::SendEnvelope(const std::string& type, const std::string& id, const std::string& payload_json) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", type.c_str());
    cJSON_AddStringToObject(root, "id", id.c_str());
    cJSON_AddStringToObject(root, "device_id", device_id_.c_str());
    cJSON_AddNumberToObject(root, "timestamp", static_cast<double>(std::time(nullptr)));
    auto payload = cJSON_Parse(payload_json.c_str());
    cJSON_AddItemToObject(root, "payload", payload ? payload : cJSON_CreateObject());
    std::string message = JsonString(root);
    cJSON_Delete(root);
    std::lock_guard<std::mutex> lock(socket_mutex_);
    if (websocket_ && websocket_->IsConnected()) websocket_->Send(message);
}

void CustomControlClient::SendHello() {
    cJSON* payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "firmware_version", esp_app_get_description()->version);
    cJSON_AddStringToObject(payload, "board", "Puppy");
    cJSON_AddNumberToObject(payload, "protocol_version", 1);
    SendEnvelope("hello", "", JsonString(payload));
    cJSON_Delete(payload);
}

void CustomControlClient::SendStatus() {
    if (!IsConnected()) return;
    SendEnvelope("status", "", Board::GetInstance().GetDeviceStatusJson());
}
