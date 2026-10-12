#if defined(ESP_PLATFORM)

// The player (docs/DESIGN.md section 4.2): a task that polls the recorder
// under the API lock, then sleeps until the time poll() asked for (a one-shot
// esp_timer, to the microsecond, where FreeRTOS's tick is 10 ms) or until
// wake() says the schedule changed (a task notification).

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "photophonique/recorder.h"

namespace photophonique {

namespace {

const char* kTag = "photophonique";

class EspClock : public PlayerClock {
public:
    uint64_t nowUs() override { return static_cast<uint64_t>(esp_timer_get_time()); }
    void wake() override {
        if (task) xTaskNotifyGive(task);
    }
    TaskHandle_t task = nullptr;
    esp_timer_handle_t timer = nullptr;
};

EspClock& clock() {
    static EspClock c;
    return c;
}

struct Player {
    tesser::Api* api;
    Recorder* recorder;
};

void run(void* arg) {
    Player* p = static_cast<Player*>(arg);
    EspClock& c = clock();
    for (;;) {
        uint64_t wait;
        {
            tesser::Lock lock(*p->api);
            wait = p->recorder->poll();
        }
        esp_timer_stop(c.timer);  // not running: an error, ignored
        if (wait == 0) continue;
        if (wait != Recorder::kIdle) esp_timer_start_once(c.timer, wait);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

}  // namespace

PlayerClock& defaultClock() { return clock(); }

void startPlayer(tesser::Api& api, Recorder& recorder) {
    static Player player{&api, &recorder};
    EspClock& c = clock();
    if (c.task) return;
    esp_timer_create_args_t t = {};
    t.callback = [](void*) { clock().wake(); };
    t.dispatch_method = ESP_TIMER_TASK;
    t.name = "player";
    if (esp_timer_create(&t, &c.timer) != ESP_OK) {
        ESP_LOGE(kTag, "no timer for the player");
        return;
    }
    // Higher than TesserKIT's task (3), so replay isn't late behind it.
    if (xTaskCreate(run, "photophonique-player", 6144, &player, 5, &c.task) != pdPASS) {
        ESP_LOGE(kTag, "no task for the player");
    }
}

}  // namespace photophonique

#endif
