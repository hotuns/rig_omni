#ifndef CUSTOM_CONTROL_CLIENT_H
#define CUSTOM_CONTROL_CLIENT_H

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>

class WebSocket;
struct cJSON;

class CustomControlClient {
public:
    static CustomControlClient& GetInstance();

    void Start();
    void Stop();
    bool IsConnected() const;

private:
    struct Job {
        std::string request_id;
        std::string response_type;
        std::string workflow_json;
    };

    CustomControlClient();
    ~CustomControlClient();
    CustomControlClient(const CustomControlClient&) = delete;
    CustomControlClient& operator=(const CustomControlClient&) = delete;

    std::unique_ptr<WebSocket> websocket_;
    QueueHandle_t job_queue_ = nullptr;
    TaskHandle_t worker_task_ = nullptr;
    esp_timer_handle_t reconnect_timer_ = nullptr;
    esp_timer_handle_t status_timer_ = nullptr;
    mutable std::mutex socket_mutex_;
    std::map<std::string, int64_t> reminder_last_run_;
    std::string server_url_;
    std::string token_;
    std::string device_id_;
    std::string workflows_json_;
    std::string reminders_json_;
    bool started_ = false;

    void LoadSettings();
    void Connect();
    void ScheduleReconnect();
    void HandleMessage(const char* data, size_t len);
    void HandleCommand(const cJSON* root);
    void HandleWorkflow(const cJSON* root);
    void SyncPayload(const cJSON* root, const char* key, std::string& target);
    void UpdateConfiguration(const cJSON* root);
    void HandleWifiList(const cJSON* root);
    void HandleWifiUpdate(const cJSON* root);
    void HandleWifiForget(const cJSON* root);
    void EnqueueJob(Job* job);
    void WorkerLoop();
    bool ExecuteWorkflow(const std::string& workflow_json, std::string& result);
    bool ExecuteToolStep(const cJSON* step, std::string& result);
    bool PlayRemoteOgg(const std::string& url, std::string& error);
    void CheckReminders();
    std::string FindWorkflow(const std::string& workflow_id) const;

    void SendEnvelope(const std::string& type, const std::string& id, const std::string& payload_json);
    void SendHello();
    void SendStatus();
};

#endif
