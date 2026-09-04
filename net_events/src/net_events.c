#include "net_events.h"

ESP_EVENT_DEFINE_BASE(NET_EVENT);

esp_err_t net_events_post(net_event_id_t id, const void *data, size_t data_len)
{
    /* Zero ticks: callers are event handlers and timer callbacks. Blocking here
     * would stall the very loop that has to drain the queue, so a full queue
     * loses the event and says so rather than deadlocking. */
    return esp_event_post(NET_EVENT, (int32_t)id, data, data_len, 0);
}

const char *net_events_name(net_event_id_t id)
{
    switch (id) {
        case NET_EVENT_LINK_UP:     return "LINK_UP";
        case NET_EVENT_LINK_DOWN:   return "LINK_DOWN";
        case NET_EVENT_AP_STARTED:  return "AP_STARTED";
        case NET_EVENT_AP_STOPPED:  return "AP_STOPPED";
        case NET_EVENT_TIME_SYNCED: return "TIME_SYNCED";
    }
    return "UNKNOWN";
}
