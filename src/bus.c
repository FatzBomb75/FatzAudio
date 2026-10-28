/* Mix buses: groups of streams on one device with their own volume and pan.
 * A bus applies its gains inside each member stream's mix (bus gain x stream
 * gain), evaluated per block on the device clock, so it needs no buffers of its
 * own and is exact to the frame. The bus list is published like the voice list:
 * an immutable array swapped atomically, freed after the callback let go. */
#include "fa_internal.h"
#include <stdlib.h>
#include <string.h>

#define FA_BUS_DEZIPPER_NS 5000000

/* Publishes a new bus list with `add` added or `remove` removed. Caller holds dev->lock. */
static fa_result publish_buses(fa_device* dev, fa_bus* add, fa_bus* remove)
{
    return fa_device_publish_list(dev, &dev->buses, add, remove);
}

FA_API fa_result FA_CALL fa_bus_create(fa_device* device, fa_bus** out_bus)
{
    fa_bus* b;
    fa_result r;
    if (!device || !out_bus) return FA_ERROR_INVALID_ARG;
    *out_bus = NULL;
    b = (fa_bus*)calloc(1, sizeof(*b));
    if (!b) return FA_ERROR_OUT_OF_MEMORY;
    b->device = device;
    fa_lane_init(&b->lanes[FA_PARAM_VOLUME], 1.0f);
    fa_lane_init(&b->lanes[FA_PARAM_PAN], 0.0f);
    fa_mutex_lock(&device->lock);
    r = publish_buses(device, b, NULL);
    fa_mutex_unlock(&device->lock);
    if (r != FA_OK) { free(b); return r; }
    *out_bus = b;
    return FA_OK;
}

FA_API void FA_CALL fa_bus_destroy(fa_bus* bus)
{
    fa_device* dev;
    fa_voice_list* voices;
    uint32_t i;
    if (!bus) return;
    dev = bus->device;
    fa_mutex_lock(&dev->lock);
    /* Members continue on the master mix. */
    voices = (fa_voice_list*)fa_load_ptr(&dev->voices);
    if (voices)
        for (i = 0; i < voices->count; i++) fa_cas_ptr(&voices->items[i]->bus, bus, NULL);
    while (publish_buses(dev, NULL, bus) != FA_OK) fa_sleep_ms(1); /* must not fail: retry on OOM */
    fa_mutex_unlock(&dev->lock);
    free(bus->buf);
    free(bus);
}

void fa_buses_free_all(fa_device* dev)
{
    fa_bus_list* list = (fa_bus_list*)fa_exchange_ptr(&dev->buses, NULL);
    uint32_t i;
    if (!list) return;
    for (i = 0; i < list->count; i++) {
        free(list->items[i]->buf);
        free(list->items[i]);
    }
    free(list);
}

FA_API fa_result FA_CALL fa_bus_set_param(fa_bus* bus, fa_param param, float value)
{
    fa_result r;
    if (!bus) return FA_ERROR_INVALID_ARG;
    r = fa_param_check(param, &value);
    if (r != FA_OK) return r;
    fa_mutex_lock(&bus->device->lock);
    fa_lane_post(&bus->lanes[param], value, FA_BUS_DEZIPPER_NS, NULL, 0, 0, -1, 0, NULL);
    fa_mutex_unlock(&bus->device->lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_bus_get_param(fa_bus* bus, fa_param param, float* out_value)
{
    float v = 0.0f;
    fa_result r;
    if (!bus || !out_value) return FA_ERROR_INVALID_ARG;
    r = fa_param_check(param, &v);
    if (r != FA_OK) return r;
    fa_mutex_lock(&bus->device->lock);
    *out_value = fa_lane_get(&bus->lanes[param]);
    fa_mutex_unlock(&bus->device->lock);
    return FA_OK;
}

FA_API fa_result FA_CALL fa_bus_ramp_param(fa_bus* bus, fa_param param, float target, fa_time duration, fa_time device_time, const fa_ramp* ramp)
{
    fa_shape shape;
    fa_result r;
    int64_t at;
    if (!bus || duration < 0 || device_time < 0) return FA_ERROR_INVALID_ARG;
    r = fa_shape_from_ramp(ramp, FA_CURVE_LINEAR, &shape);
    if (r != FA_OK) return r;
    r = fa_param_check(param, &target);
    if (r != FA_OK) return r;
    at = fa_ramp_frame(bus->device->rate, device_time);
    fa_mutex_lock(&bus->device->lock);
    fa_lane_post(&bus->lanes[param], target, duration, &shape, 0, 1, at, 0, NULL);
    fa_mutex_unlock(&bus->device->lock);
    return FA_OK;
}

void fa_buses_begin(fa_device* dev)
{
    fa_bus_list* list = (fa_bus_list*)fa_load_ptr(&dev->buses);
    uint32_t i;
    if (!list) return;
    for (i = 0; i < list->count; i++) fa_lanes_take(list->items[i]->lanes, FA_MIX_PARAMS, dev->rate);
}

void fa_buses_end(fa_device* dev, uint32_t frames)
{
    fa_bus_list* list = (fa_bus_list*)fa_load_ptr(&dev->buses);
    int64_t clock = fa_load64(&dev->clock);
    uint32_t i;
    if (!list) return;
    for (i = 0; i < list->count; i++) fa_lanes_end(list->items[i]->lanes, FA_MIX_PARAMS, clock, frames);
}
