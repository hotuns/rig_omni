        #include "emote_display.h"

// Standard C++ headers
#include <cstring>
#include <memory>
#include <unordered_map>
#include <tuple>
#include <algorithm>
#include <cinttypes>
#include <cmath>

// Standard C headers
#include <sys/time.h>
#include <time.h>

// ESP-IDF headers
#include <esp_log.h>
#include <esp_lcd_panel_io.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <lvgl.h>

// FreeRTOS headers
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Project headers
#include "assets/lang_config.h"
#include "assets.h"
#include "board.h"
#include "gfx.h"
#include "expression_emote.h"


namespace emote {

// ============================================================================
// Constants and Type Definitions
// ============================================================================

static const char* TAG = "EmoteDisplay";

static constexpr uint16_t kEyeColor = 0xD7BD;
static constexpr uint16_t kPinkColor = 0xFB91;
static constexpr uint16_t kTearColor = 0x5DBF;

struct EyeSpec {
    const char* name;
    float width;
    float height;
    float gap;
    float tilt;
    float asymmetry;
    float top_radius;
    float bottom_radius;
};

static const EyeSpec kEyeSpecs[] = {
    {"neutral", 54, 58, 32, 0, 0, .25f, .25f},
    {"happy", 58, 48, 30, 0, 0, .16f, .56f},
    {"laughing", 62, 26, 28, 0, 0, .12f, .68f},
    {"loving", 54, 66, 30, 0, 0, .42f, .50f},
    {"angry", 52, 34, 28, .25f, 0, .10f, .28f},
    {"sad", 56, 38, 30, -.32f, 0, .28f, .58f},
    {"crying", 56, 40, 30, -.34f, 0, .28f, .60f},
    {"confused", 50, 48, 34, .05f, .18f, .22f, .42f},
    {"cool", 58, 22, 30, .04f, 0, .16f, .16f},
    {"shocked", 42, 84, 34, 0, 0, .50f, .50f},
    {"surprised", 60, 66, 30, 0, 0, .42f, .48f},
    {"sleepy", 58, 14, 34, 0, 0, .50f, .50f},
    {"winking", 54, 58, 30, 0, 0, .36f, .52f},
    {"silly", 52, 54, 32, .10f, -.25f, .42f, .58f},
    {"relaxed", 60, 28, 32, 0, 0, .42f, .62f},
    {"thinking", 48, 46, 36, 0, .20f, .28f, .50f},
    {"listen", 60, 44, 34, 0, 0, .22f, .46f},
    {"confident", 58, 28, 30, -.10f, 0, .12f, .26f},
    {"embarrassed", 46, 48, 36, 0, 0, .42f, .56f},
    {"kiss", 54, 24, 32, 0, 0, .44f, .64f},
    {"delicious", 58, 30, 30, 0, 0, .16f, .64f},
};

static const EyeSpec* FindEyeSpec(const char* name)
{
    for (const auto& spec : kEyeSpecs) {
        if (strcmp(spec.name, name) == 0) {
            return &spec;
        }
    }
    return nullptr;
}

static inline uint16_t PixelColor(uint16_t rgb565)
{
    return __builtin_bswap16(rgb565);
}

static void PutPixel(uint16_t* buffer, int width, int height, int x, int y, uint16_t color)
{
    if (x >= 0 && x < width && y >= 0 && y < height) {
        buffer[y * width + x] = PixelColor(color);
    }
}

static void FillCircle(uint16_t* buffer, int width, int height, float cx, float cy, float radius, uint16_t color)
{
    const int x0 = std::max(0, static_cast<int>(floorf(cx - radius)));
    const int x1 = std::min(width - 1, static_cast<int>(ceilf(cx + radius)));
    const int y0 = std::max(0, static_cast<int>(floorf(cy - radius)));
    const int y1 = std::min(height - 1, static_cast<int>(ceilf(cy + radius)));
    const float radius_sq = radius * radius;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float dx = x + 0.5f - cx;
            const float dy = y + 0.5f - cy;
            if (dx * dx + dy * dy <= radius_sq) {
                PutPixel(buffer, width, height, x, y, color);
            }
        }
    }
}

static void DrawLine(uint16_t* buffer, int width, int height, float x0, float y0, float x1, float y1,
                     float thickness, uint16_t color)
{
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const int steps = std::max(1, static_cast<int>(ceilf(std::max(fabsf(dx), fabsf(dy)))));
    for (int i = 0; i <= steps; ++i) {
        const float p = static_cast<float>(i) / steps;
        FillCircle(buffer, width, height, x0 + dx * p, y0 + dy * p, thickness * 0.5f, color);
    }
}

static void DrawArc(uint16_t* buffer, int width, int height, float cx, float cy, float radius,
                    float start_angle, float end_angle, float thickness, uint16_t color)
{
    for (int i = 0; i <= 24; ++i) {
        const float angle = start_angle + (end_angle - start_angle) * i / 24.0f;
        FillCircle(buffer, width, height, cx + cosf(angle) * radius,
                   cy + sinf(angle) * radius, thickness * 0.5f, color);
    }
}

static void DrawQuestionMark(uint16_t* buffer, int width, int height, float x, float y,
                             float scale, uint16_t color)
{
    DrawArc(buffer, width, height, x, y + 8 * scale, 9 * scale,
            static_cast<float>(M_PI) + 0.32f, static_cast<float>(M_PI * 2) - 0.1f,
            3 * scale, color);
    DrawLine(buffer, width, height, x + 8.5f * scale, y + 9 * scale,
             x + 2 * scale, y + 19 * scale, 3 * scale, color);
    FillCircle(buffer, width, height, x + 2 * scale, y + 27 * scale, 2.2f * scale, color);
}

static void DrawZ(uint16_t* buffer, int width, int height, float x, float y,
                  float size, float thickness, uint16_t color)
{
    DrawLine(buffer, width, height, x, y, x + size, y, thickness, color);
    DrawLine(buffer, width, height, x + size, y, x, y + size, thickness, color);
    DrawLine(buffer, width, height, x, y + size, x + size, y + size, thickness, color);
}

static uint16_t ScaleColor(uint16_t color, float factor)
{
    const uint16_t red = static_cast<uint16_t>(((color >> 11) & 0x1F) * factor);
    const uint16_t green = static_cast<uint16_t>(((color >> 5) & 0x3F) * factor);
    const uint16_t blue = static_cast<uint16_t>((color & 0x1F) * factor);
    return static_cast<uint16_t>((red << 11) | (green << 5) | blue);
}

static void FillRoundedRect(uint16_t* buffer, int width, int height, float cx, float cy, float rect_w,
                            float rect_h, float top_radius, float bottom_radius, float angle,
                            uint16_t color)
{
    const float bound = hypotf(rect_w, rect_h) * 0.5f + 2;
    const int x0 = std::max(0, static_cast<int>(floorf(cx - bound)));
    const int x1 = std::min(width - 1, static_cast<int>(ceilf(cx + bound)));
    const int y0 = std::max(0, static_cast<int>(floorf(cy - bound)));
    const int y1 = std::min(height - 1, static_cast<int>(ceilf(cy + bound)));
    const float cosine = cosf(angle);
    const float sine = sinf(angle);
    const float half_w = rect_w * 0.5f;
    const float half_h = rect_h * 0.5f;
    top_radius = std::min(top_radius, std::min(half_w, half_h));
    bottom_radius = std::min(bottom_radius, std::min(half_w, half_h));

    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float px = x + 0.5f - cx;
            const float py = y + 0.5f - cy;
            const float local_x = cosine * px + sine * py;
            const float local_y = -sine * px + cosine * py;
            const float radius = local_y < 0 ? top_radius : bottom_radius;
            const float qx = fabsf(local_x) - (half_w - radius);
            const float qy = fabsf(local_y) - (half_h - radius);
            const float outside = hypotf(std::max(qx, 0.0f), std::max(qy, 0.0f));
            const float inside = std::min(std::max(qx, qy), 0.0f);
            if (outside + inside <= radius) {
                PutPixel(buffer, width, height, x, y, color);
            }
        }
    }
}

// ============================================================================
// Forward Declarations
// ============================================================================

class EmoteDisplay;

// ============================================================================
// Helper Functions
// ============================================================================

static bool OnFlushIoReady(const esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t* const edata, void* user_ctx)
{
    emote_handle_t handle = static_cast<emote_handle_t>(user_ctx);
    if (handle) {
        emote_notify_flush_finished(handle);
    }
    return true;
}

// Flush callback for emote
static void OnFlushCallback(int x_start, int y_start, int x_end, int y_end, const void* data, emote_handle_t handle)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)emote_get_user_data(handle);
    if (panel != nullptr) {
        esp_lcd_panel_draw_bitmap(panel, x_start, y_start, x_end, y_end, data);
    }
}

// ============================================================================
// Graphics Initialization Functions
// ============================================================================

static emote_handle_t InitializeEmote(const esp_lcd_panel_handle_t panel, const int width, const int height)
{
    if (!panel) {
        ESP_LOGE(TAG, "Invalid panel");
        return nullptr;
    }

    emote_config_t emote_cfg = {
        .flags = {
            .swap = true,
            .double_buffer = true,
            .buff_dma = false,
        },
        .gfx_emote = {
            .h_res = width,
            .v_res = height,
            .fps = 30,
        },
        .buffers = {
            .buf_pixels = static_cast<size_t>(width * 16),
        },
        .task = {
            .task_priority = 5,
            .task_stack = 6 * 1024,
            .task_affinity = 0,
            .task_stack_in_ext = false,
        },
        .flush_cb = OnFlushCallback,
        .user_data = (void*)panel,
    };

    emote_handle_t emote_handle = emote_init(&emote_cfg);
    if (!emote_handle) {
        ESP_LOGE(TAG, "Failed to initialize emote");
        return nullptr;
    }

    return emote_handle;
}

// ============================================================================
// EmoteDisplay Class Implementation
// ============================================================================

EmoteDisplay::EmoteDisplay(const esp_lcd_panel_handle_t panel, const esp_lcd_panel_io_handle_t panel_io,
                           const int width, const int height, const bool parametric_eyes)
{
    width_ = width;
    height_ = height;
    parametric_eyes_enabled_ = parametric_eyes;
    emote_handle_ = InitializeEmote(panel, width, height);

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = OnFlushIoReady,
    };
    esp_lcd_panel_io_register_event_callbacks(panel_io, &cbs, emote_handle_);
}

EmoteDisplay::~EmoteDisplay()
{
    StopParametricEyes();
    if (parametric_timer_) {
        esp_timer_delete(parametric_timer_);
        parametric_timer_ = nullptr;
    }
    if (parametric_buffer_) {
        heap_caps_free(parametric_buffer_);
        parametric_buffer_ = nullptr;
    }
    // 释放预览定时器
    if (preview_timer_) {
        esp_timer_stop(preview_timer_);
        esp_timer_delete(preview_timer_);
        preview_timer_ = nullptr;
    }
    // 释放预览数据缓冲区
    if (preview_data_) {
        heap_caps_free(preview_data_);
        preview_data_ = nullptr;
        preview_data_size_ = 0;
    }
    // 释放 emote 句柄
    if (emote_handle_) {
        emote_deinit(emote_handle_);
        emote_handle_ = nullptr;
    }
}

void EmoteDisplay::SetEmotion(const char* const emotion)
{
    ESP_LOGI(TAG, "SetEmotion: %s", emotion);
    if (emote_handle_ && emotion && strlen(emotion) > 0) {
        if (parametric_eyes_enabled_ && IsParametricEmotion(emotion)) {
            if (StartParametricEyes(emotion)) {
                return;
            }
            ESP_LOGW(TAG, "Parametric eye renderer unavailable, falling back to EAF");
        }
        StopParametricEyes();
        emote_set_anim_emoji(emote_handle_, emotion);
    }
}

void EmoteDisplay::SetGaze(float x, float y)
{
    manual_gaze_ = true;
    gaze_target_x_ = std::clamp(x, -18.0f, 18.0f);
    gaze_target_y_ = std::clamp(y, -8.0f, 8.0f);
}

void EmoteDisplay::ClearGaze()
{
    manual_gaze_ = false;
    gaze_target_x_ = 0;
    gaze_target_y_ = 0;
    next_gaze_us_ = esp_timer_get_time() + 700000;
}

bool EmoteDisplay::IsParametricEmotion(const char* emotion) const
{
    return emotion && FindEyeSpec(emotion) != nullptr;
}

bool EmoteDisplay::StartParametricEyes(const char* emotion)
{
    if (!emote_handle_ || !emotion) {
        return false;
    }

    if (!parametric_buffer_) {
        const size_t data_size = static_cast<size_t>(width_) * height_ * sizeof(uint16_t);
        parametric_buffer_ = static_cast<uint16_t*>(
            heap_caps_calloc(static_cast<size_t>(width_) * height_, sizeof(uint16_t),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!parametric_buffer_) {
            ESP_LOGE(TAG, "Failed to allocate parametric eye buffer");
            return false;
        }

        parametric_obj_ = emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_IMAGE, "parametric_eyes");
        if (!parametric_obj_) {
            ESP_LOGE(TAG, "Failed to create parametric eye image object");
            heap_caps_free(parametric_buffer_);
            parametric_buffer_ = nullptr;
            return false;
        }

        parametric_img_dsc_.header.magic = 0x19;
        parametric_img_dsc_.header.cf = GFX_COLOR_FORMAT_RGB565;
        parametric_img_dsc_.header.w = width_;
        parametric_img_dsc_.header.h = height_;
        parametric_img_dsc_.header.stride = width_ * sizeof(uint16_t);
        parametric_img_dsc_.data = reinterpret_cast<const uint8_t*>(parametric_buffer_);
        parametric_img_dsc_.data_size = data_size;

        emote_lock(emote_handle_);
        gfx_img_set_src(parametric_obj_, &parametric_img_dsc_);
        gfx_obj_align(parametric_obj_, GFX_ALIGN_CENTER, 0, 0);
        emote_unlock(emote_handle_);
    }

    if (!parametric_timer_) {
        const esp_timer_create_args_t timer_args = {
            .callback = ParametricTimerCallback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "parametric_eyes",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&timer_args, &parametric_timer_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create parametric eye timer");
            return false;
        }
    }

    const int64_t now = esp_timer_get_time();
    parametric_emotion_ = emotion;
    parametric_start_us_ = now;
    next_blink_us_ = now + 1200000;
    blink_start_us_ = 0;
    next_gaze_us_ = now + 900000;
    gaze_x_ = 0;
    gaze_y_ = 0;
    gaze_target_x_ = 0;
    gaze_target_y_ = 0;
    manual_gaze_ = false;
    parametric_eyes_active_ = true;

    emote_lock(emote_handle_);
    gfx_obj_set_visible(parametric_obj_, true);
    emote_unlock(emote_handle_);
    RenderParametricEyes(now);
    esp_timer_stop(parametric_timer_);
    esp_timer_start_periodic(parametric_timer_, 33333);
    return true;
}

void EmoteDisplay::StopParametricEyes()
{
    parametric_eyes_active_ = false;
    if (parametric_timer_) {
        esp_timer_stop(parametric_timer_);
    }
    if (emote_handle_ && parametric_obj_) {
        emote_lock(emote_handle_);
        gfx_obj_set_visible(parametric_obj_, false);
        emote_unlock(emote_handle_);
        emote_notify_all_refresh(emote_handle_);
    }
}

void EmoteDisplay::ParametricTimerCallback(void* arg)
{
    auto* display = static_cast<EmoteDisplay*>(arg);
    if (display && display->parametric_eyes_active_) {
        display->RenderParametricEyes(esp_timer_get_time());
    }
}

void EmoteDisplay::RenderParametricEyes(int64_t now_us)
{
    if (!parametric_eyes_active_ || !parametric_buffer_ || !emote_handle_) {
        return;
    }
    const EyeSpec* spec = FindEyeSpec(parametric_emotion_.c_str());
    if (!spec) {
        return;
    }

    memset(parametric_buffer_, 0, static_cast<size_t>(width_) * height_ * sizeof(uint16_t));
    const float t = now_us / 1000000.0f;
    const float age_ms = (now_us - parametric_start_us_) / 1000.0f;
    const float entrance = std::min(1.0f, age_ms / 620.0f);
    const float entrance_ease = 1 - powf(1 - entrance, 3);
    const float settle = sinf(age_ms / 42.0f) * expf(-age_ms / 230.0f);

    if (!manual_gaze_ && now_us >= next_gaze_us_) {
        gaze_target_x_ = (static_cast<int32_t>(esp_random() % 1001) - 500) / 100.0f;
        gaze_target_y_ = (static_cast<int32_t>(esp_random() % 601) - 300) / 120.0f;
        next_gaze_us_ = now_us + 900000 + esp_random() % 1900000;
    }
    gaze_x_ += (gaze_target_x_ - gaze_x_) * 0.075f;
    gaze_y_ += (gaze_target_y_ - gaze_y_) * 0.075f;

    if (now_us >= next_blink_us_ && blink_start_us_ == 0) {
        blink_start_us_ = now_us;
        next_blink_us_ = now_us + 1400000 + esp_random() % 2600000;
    }
    float blink = 1;
    if (blink_start_us_ != 0) {
        const float progress = (now_us - blink_start_us_) / 210000.0f;
        if (progress >= 1) {
            blink_start_us_ = 0;
        } else {
            blink = std::max(0.08f, fabsf(progress * 2 - 1));
        }
    }

    const bool happy = parametric_emotion_ == "happy";
    const bool laughing = parametric_emotion_ == "laughing";
    const bool loving = parametric_emotion_ == "loving";
    const bool angry = parametric_emotion_ == "angry";
    const bool sad = parametric_emotion_ == "sad" || parametric_emotion_ == "crying";
    const bool confused = parametric_emotion_ == "confused";
    const bool shocked = parametric_emotion_ == "shocked";
    const bool surprised = parametric_emotion_ == "surprised";
    const bool sleepy = parametric_emotion_ == "sleepy";
    const bool relaxed = parametric_emotion_ == "relaxed";
    const bool silly = parametric_emotion_ == "silly";
    const bool listening = parametric_emotion_ == "listen";

    for (int index = 0; index < 2; ++index) {
        const float side = index == 0 ? -1.0f : 1.0f;
        float eye_w = spec->width * (0.72f + entrance_ease * 0.28f + settle * 0.08f);
        float eye_h = spec->height * blink * (0.42f + entrance_ease * 0.58f - settle * 0.18f);
        float offset_x = gaze_x_;
        float offset_y = gaze_y_;
        float angle = index == 0 ? spec->tilt : -spec->tilt;

        if (spec->asymmetry != 0) {
            const float factor = index == 0 ? 1 + spec->asymmetry : 1 - spec->asymmetry;
            eye_h *= factor;
            eye_w *= 1 + (1 - factor) * 0.35f;
        }
        if (happy) {
            offset_y += sinf(t * 3.1f) * 2.1f;
            eye_w *= 1 + sinf(t * 3.1f) * 0.035f;
            angle += side * sinf(t * 3.1f) * 0.018f;
        } else if (laughing) {
            const float laugh = fabsf(sinf(t * 5.8f));
            offset_y -= laugh * 5.5f;
            eye_w *= 1 + laugh * 0.07f;
            eye_h *= 1 - laugh * 0.16f;
            angle += side * laugh * 0.035f;
        } else if (loving) {
            eye_w *= 1 + sinf(t * 2.5f) * 0.045f;
            eye_h *= 1 + sinf(t * 2.5f) * 0.065f;
            offset_x -= side * (2 + sinf(t * 2.5f));
        } else if (angry) {
            offset_x -= side * (2.4f + sinf(t * 7) * 0.8f);
            eye_h *= 1 - fabsf(sinf(t * 7)) * 0.04f;
        } else if (sad) {
            offset_y += 2.5f + sinf(t * 1.15f + index * 0.7f) * 1.3f;
        } else if (confused) {
            offset_y += side * sinf(t * 1.8f) * 3.2f;
            angle += side * sinf(t * 1.8f) * 0.045f;
        } else if (shocked) {
            eye_h *= 1 + sinf(t * 3.8f) * 0.045f;
            offset_y -= fabsf(sinf(t * 3.8f)) * 2;
        } else if (surprised) {
            eye_w *= 1 + sinf(t * 3.3f) * 0.035f;
            eye_h *= 1 + sinf(t * 3.3f) * 0.07f;
            offset_y += sinf(t * 3.3f) * 2.8f;
        } else if (sleepy || relaxed) {
            offset_y += sinf(t * 0.72f + index * 0.3f) * 1.2f;
        } else if (listening) {
            const float focus = sinf(t * 3.2f);
            eye_h *= 1 + focus * 0.12f;
            eye_w *= 1 - focus * 0.04f;
            offset_x += sinf(t * 1.6f) * 5;
            offset_y -= fabsf(focus) * 2;
        } else if (silly) {
            eye_h *= index == 0 ? 0.72f : 1.16f;
            offset_x += side * 7;
            angle += side * -0.12f;
        }

        const float center_x = 120 + side * (spec->gap * 0.5f + spec->width * 0.5f) + offset_x;
        const float center_y = 120 + offset_y;
        if (parametric_emotion_ == "winking" && index == 1) {
            eye_h = 10;
        }
        const float radius_limit = std::min(eye_w, eye_h) * 0.5f;
        FillRoundedRect(parametric_buffer_, width_, height_, center_x, center_y, eye_w,
                        std::max(5.0f, eye_h),
                        std::min(radius_limit, radius_limit * spec->top_radius * 2),
                        std::min(radius_limit, radius_limit * spec->bottom_radius * 2), angle,
                        kEyeColor);
    }

    if (listening) {
        for (int index = 0; index < 3; ++index) {
            const float phase = fmodf(t * 1.15f + index / 3.0f, 1.0f);
            const float radius = 72 + phase * 34;
            const float brightness = 0.25f + (1 - phase) * 0.75f;
            const float thickness = 1.5f + (1 - phase) * 1.5f;
            const uint16_t color = ScaleColor(kEyeColor, brightness);
            DrawArc(parametric_buffer_, width_, height_, 120, 120, radius, -0.5f, 0.5f,
                    thickness, color);
            DrawArc(parametric_buffer_, width_, height_, 120, 120, radius,
                    static_cast<float>(M_PI) - 0.5f, static_cast<float>(M_PI) + 0.5f,
                    thickness, color);
        }
    }

    if (confused) {
        const float wobble = sinf(t * 2.2f) * 2.0f;
        DrawQuestionMark(parametric_buffer_, width_, height_, 176 + wobble, 45, 1.15f, kEyeColor);
    }
    if (sleepy) {
        const float rise = fmodf(t * 7.0f, 7.0f);
        DrawZ(parametric_buffer_, width_, height_, 154, 72 - rise * 0.35f, 10, 2.4f, kEyeColor);
        DrawZ(parametric_buffer_, width_, height_, 169, 57 - rise * 0.5f, 13, 2.7f, kEyeColor);
        DrawZ(parametric_buffer_, width_, height_, 187, 40 - rise * 0.65f, 16, 3.0f, kEyeColor);
    }

    const float mouth_y = 171;
    if (shocked) {
        FillCircle(parametric_buffer_, width_, height_, 120, mouth_y, 6, kEyeColor);
        FillCircle(parametric_buffer_, width_, height_, 120, mouth_y, 3, 0x0000);
    } else if (surprised) {
        FillCircle(parametric_buffer_, width_, height_, 120, mouth_y, 4, kEyeColor);
    } else if (silly) {
        DrawLine(parametric_buffer_, width_, height_, 113, mouth_y, 127, mouth_y, 3, kEyeColor);
        FillCircle(parametric_buffer_, width_, height_, 123, mouth_y + 4, 3, kPinkColor);
    } else if (parametric_emotion_ == "kiss") {
        DrawLine(parametric_buffer_, width_, height_, 115, mouth_y, 120, mouth_y - 4, 2, kPinkColor);
        DrawLine(parametric_buffer_, width_, height_, 120, mouth_y - 4, 125, mouth_y, 2, kPinkColor);
        DrawLine(parametric_buffer_, width_, height_, 115, mouth_y, 120, mouth_y + 4, 2, kPinkColor);
        DrawLine(parametric_buffer_, width_, height_, 120, mouth_y + 4, 125, mouth_y, 2, kPinkColor);
    }
    if (parametric_emotion_ == "crying") {
        const float tear_y = 158 + (sinf(t * 3.1f) + 1) * 6;
        FillCircle(parametric_buffer_, width_, height_, 67, tear_y, 4, kTearColor);
        FillCircle(parametric_buffer_, width_, height_, 173, tear_y, 4, kTearColor);
    }

    emote_lock(emote_handle_);
    gfx_obj_set_visible(parametric_obj_, true);
    emote_notify_all_refresh(emote_handle_);
    emote_unlock(emote_handle_);
}

void EmoteDisplay::SetChatMessage(const char* const role, const char* const content)
{
    // 不显示对话内容，直接返回
    (void)role;
    (void)content;
}

void EmoteDisplay::SetStatus(const char* const status)
{
    ESP_LOGI(TAG, "SetStatus: %s", status);
    if (emote_handle_ && status && strlen(status) > 0) {
        // 统一使用 EMOTE_MGR_EVT_IDLE，避免设置 icon (没有 icon 资源)
        // LISTENING 原本用 EMOTE_MGR_EVT_LISTEN (设置 icon_mic)
        // SPEAKING 原本用 EMOTE_MGR_EVT_SPEAK (设置 icon_speaker)
        emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_IDLE, NULL);
    }
}

void EmoteDisplay::ShowNotification(const char* notification, int duration_ms)
{
    ESP_LOGI(TAG, "ShowNotification: %s", notification);
    if (emote_handle_ && notification && strlen(notification) > 0) {
        // 使用 EMOTE_MGR_EVT_IDLE 避免设置 icon_tips (没有 icon 资源)
        emote_set_event_msg(emote_handle_, EMOTE_MGR_EVT_IDLE, notification);
    }
}

void EmoteDisplay::UpdateStatusBar(bool update_all)
{
    ESP_LOGD(TAG, "UpdateStatusBar: %s", update_all ? "true" : "false");
    if (!emote_handle_) {
        return;
    }
}

void EmoteDisplay::SetPowerSaveMode(bool on)
{
    ESP_LOGI(TAG, "SetPowerSaveMode: %s", on ? "ON" : "OFF");
    if (!emote_handle_) {
        return;
    }
}

void EmoteDisplay::SetPreviewImage(const void* image)
{
    if (image) {
        ESP_LOGI(TAG, "SetPreviewImage: Preview image not supported, using default icon");
    }
}

void EmoteDisplay::SetPreviewRgb565(const void* data, int width, int height, int stride)
{
    if (!emote_handle_) {
        return;
    }

    // 如果 data 为空，隐藏预览图像
    if (data == nullptr) {
        if (preview_obj_) {
            emote_lock(emote_handle_);
            gfx_obj_set_visible(preview_obj_, false);
            emote_unlock(emote_handle_);
        }
        if (preview_timer_) {
            esp_timer_stop(preview_timer_);
        }
        ESP_LOGI(TAG, "SetPreviewRgb565: Hide preview");
        return;
    }

    // 先隐藏旧预览，再更新数据后显示，强制 GFX 引擎重新渲染以避免显示旧图像
    emote_lock(emote_handle_);
    if (preview_obj_) {
        gfx_obj_set_visible(preview_obj_, false);
    }
    emote_unlock(emote_handle_);

    // 创建预览图像对象（如果不存在）
    if (!preview_obj_) {
        preview_obj_ = emote_create_obj_by_type(emote_handle_, EMOTE_OBJ_TYPE_IMAGE, "camera_preview");
        if (!preview_obj_) {
            ESP_LOGE(TAG, "Failed to create preview image object");
            return;
        }

        // 创建预览隐藏定时器
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                auto display = static_cast<EmoteDisplay*>(arg);
                if (display && display->preview_obj_ && display->emote_handle_) {
                    emote_lock(display->emote_handle_);
                    gfx_obj_set_visible(display->preview_obj_, false);
                    emote_unlock(display->emote_handle_);
                }
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "preview_timer",
            .skip_unhandled_events = true,
        };
        esp_timer_create(&timer_args, &preview_timer_);
    }

    // 计算数据大小并复制数据（需要字节交换 RGB565 LE -> BE，LCD 需要大端序）
    size_t data_size = height * stride;
    if (preview_data_size_ < data_size) {
        // 重新分配缓冲区
        if (preview_data_) {
            heap_caps_free(preview_data_);
        }
        preview_data_ = (uint8_t*)heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!preview_data_) {
            ESP_LOGE(TAG, "Failed to allocate preview data buffer");
            preview_data_size_ = 0;
            return;
        }
        preview_data_size_ = data_size;
    }
    
    // RGB565 字节交换: 高低字节交换 (LCD 需要大端序)
    const uint16_t* src = reinterpret_cast<const uint16_t*>(data);
    uint16_t* dst = reinterpret_cast<uint16_t*>(preview_data_);
    size_t pixel_count = width * height;
    for (size_t i = 0; i < pixel_count; i++) {
        dst[i] = __builtin_bswap16(src[i]);
    }

    // 设置图像描述符
    preview_img_dsc_.header.magic = 0x19;  // C_ARRAY_HEADER_MAGIC
    preview_img_dsc_.header.cf = GFX_COLOR_FORMAT_RGB565;
    preview_img_dsc_.header.w = width;
    preview_img_dsc_.header.h = height;
    preview_img_dsc_.header.stride = stride;
    preview_img_dsc_.data = preview_data_;
    preview_img_dsc_.data_size = data_size;

    // 设置图像并显示
    emote_lock(emote_handle_);
    gfx_img_set_src(preview_obj_, &preview_img_dsc_);
    gfx_obj_align(preview_obj_, GFX_ALIGN_CENTER, 0, 0);
    gfx_obj_set_visible(preview_obj_, true);
    emote_unlock(emote_handle_);

    // 启动定时器，5秒后自动隐藏
    esp_timer_stop(preview_timer_);
    esp_timer_start_once(preview_timer_, 5000000);  // 5秒

    ESP_LOGI(TAG, "SetPreviewRgb565: %dx%d, stride=%d", width, height, stride);
}

void EmoteDisplay::SetTheme(Theme* const theme)
{
    ESP_LOGI(TAG, "SetTheme: %p", theme);
}

bool EmoteDisplay::Lock(const int timeout_ms)
{
    (void)timeout_ms;
    return true;
}

void EmoteDisplay::Unlock()
{
}

bool EmoteDisplay::StopAnimDialog()
{
    ESP_LOGI(TAG, "StopAnimDialog");
    if (emote_handle_) {
        return emote_stop_anim_dialog(emote_handle_);
    }
    return false;
}

bool EmoteDisplay::InsertAnimDialog(const char* emoji_name, uint32_t duration_ms)
{
    ESP_LOGI(TAG, "InsertAnimDialog: %s, %" PRIu32, emoji_name, duration_ms);
    if (emote_handle_ && emoji_name) {
        return emote_insert_anim_dialog(emote_handle_, emoji_name, duration_ms);
    }
    return false;
}

void EmoteDisplay::RefreshAll()
{
    if (emote_handle_) {
        emote_notify_all_refresh(emote_handle_);
        return;
    }
}

} // namespace emote
