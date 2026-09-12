#include "json_util.h"

#include <stdio.h>

const char* json_string_or(const cJSON* obj, const char* key) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

int json_int_or(const cJSON* obj, const char* key, int fallback) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? (int)item->valuedouble : fallback;
}

cJSON* json_float_item(float value) {
    // 9 significant digits, a sign, a point, an exponent and a terminator fit
    // inside 32 with room to spare; snprintf truncates rather than overruns if
    // that is ever wrong.
    char text[32];
    snprintf(text, sizeof(text), "%.9g", (double)value);
    return cJSON_CreateRaw(text);
}

bool json_add_float(cJSON* obj, const char* key, float value) {
    if (!obj || !key)
        return false;
    cJSON* item = json_float_item(value);
    if (!item)
        return false;
    if (!cJSON_AddItemToObject(obj, key, item)) {
        cJSON_Delete(item);
        return false;
    }
    return true;
}
