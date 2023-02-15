#include "Context.h"

#include <utility>
#include <string.h>
#include <iostream>
#include <thread>

Context::Context(const std::string& name, JSRuntime *rt, JNIEnv* env) {
    this->ctx = JS_NewContext(rt);
    this->name = name;
    this->env = env;

    this->init_api_console();
    this->init_api_event_listeners();
    this->init_api_crypto();
    this->init_api_timeout();

    JS_SetContextOpaque(this->ctx, this);

    JSValue global_obj = JS_GetGlobalObject(this->ctx);
    this->global_json_obj = JS_GetPropertyStr(this->ctx, global_obj, "JSON");
    JS_FreeValue(this->ctx, global_obj);
}

Context::~Context() {
    this->stop_run_loop();

    for(const auto& kv: this->event_listeners) {
        JS_FreeValue(this->ctx, kv.second);
    }

    for(const auto& kv: this->timers) {
        JS_FreeValue(this->ctx, kv.second.js_func);
    }

    JS_FreeValue(this->ctx, this->global_json_obj);
    JS_FreeContext(this->ctx);
}

void Context::start_run_loop() {
    this->end_run_loop = false;

    this->run_loop_thread = std::thread([this] {
       run_loop();
    });

    this->run_loop_thread.detach();

    this->run_loop_stopped = false;
}

void Context::stop_run_loop() {
    if (this->end_run_loop) {
        return;
    }

    this->run_loop_mutex.lock();
    this->end_run_loop = true;
    this->run_loop_mutex.unlock();

    while(!this->run_loop_stopped) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void Context::run_loop()
{
    for(;;) {
        this->run_loop_mutex.lock();
        if (this->end_run_loop) {
            break;
        }
        this->run_loop_mutex.unlock();

        if (!this->timers.empty()) {
            this->timers_mutex.lock();
            JSValue global_obj = JS_GetGlobalObject(this->ctx);

            for(const auto& kv: this->timers) {
                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - kv.second.start);

                if (duration.count() >= kv.second.timeout) {
                    this->timers[kv.first].start = std::chrono::system_clock::now();

                    JSValue value = JS_Call(this->ctx, this->timers[kv.first].js_func, global_obj, 0, nullptr);
                    JS_FreeValue(this->ctx, value);

                    if (!this->timers[kv.first].repeat) {
                        JS_FreeValue(this->ctx, this->timers[kv.first].js_func);
                        this->timers.erase(kv.first);
                    }
                }
            }

            this->timers_mutex.unlock();

            JS_FreeValue(this->ctx, global_obj);
        }
    }
    
    this->run_loop_stopped = true;
}

JSValue Context::parseJSON(const char *json_string)
{
    JSValue ret_value = JS_UNDEFINED;

    JSValue global_obj = JS_GetGlobalObject(this->ctx);
    JSValue parse_func_obj = JS_GetPropertyStr(this->ctx, this->global_json_obj, "parse");

    JSValue json = JS_NewString(this->ctx, json_string);

    JSValue parsed = JS_Call(this->ctx, parse_func_obj, global_obj, 1, &json);
    ret_value = JS_DupValue(this->ctx, parsed);

    JS_FreeValue(this->ctx, parsed);
    JS_FreeValue(this->ctx, json);
    JS_FreeValue(this->ctx, parse_func_obj);
    JS_FreeValue(this->ctx, global_obj);

    return ret_value;
}

JSValue Context::evaluate(const char * code) {
    int flags = JS_EVAL_TYPE_GLOBAL;
    flags |= JS_EVAL_FLAG_BACKTRACE_BARRIER;

    return JS_Eval(this->ctx, code, strlen(code), "<code>", flags);
}

JSValue Context::dispatch_event(const std::string &event, JSValue details) {
    JSValue ret_value = JS_UNDEFINED;
    JSValue global_obj = JS_GetGlobalObject(this->ctx);

    auto range = this->event_listeners.equal_range(event);
    for (auto itr = range.first; itr != range.second; ++itr) {
        auto callback = (JSValue)itr->second;

        JSValue value = JS_Call(this->ctx, callback, global_obj, 1, &details);
        if (JS_IsException(value)) {
            ret_value = JS_DupValue(this->ctx, value);
            JS_FreeValue(this->ctx, value);
            break;
        }

        JS_FreeValue(this->ctx, value);
    }

    JS_FreeValue(this->ctx, global_obj);
    return ret_value;
}

void Context::init_api_console()
{
    JSValue global_obj, console;

    global_obj = JS_GetGlobalObject(this->ctx);
    console = JS_NewObject(this->ctx);

    JS_SetPropertyStr(this->ctx, console, "log", JS_NewCFunction(ctx, api_console_log, "log", 1));
    JS_SetPropertyStr(this->ctx, console, "info", JS_NewCFunction(ctx, api_console_log, "info", 1));
    JS_SetPropertyStr(this->ctx, console, "warn", JS_NewCFunction(ctx, api_console_warn, "warn", 1));
    JS_SetPropertyStr(this->ctx, console, "error", JS_NewCFunction(ctx, api_console_error, "error", 1));
    JS_SetPropertyStr(this->ctx, console, "debug", JS_NewCFunction(ctx, api_console_debug, "debug", 1));

    JS_SetPropertyStr(this->ctx, global_obj, "console", console);

    JS_FreeValue(this->ctx, global_obj);
}

void Context::init_api_event_listeners()
{
    JSValue global_obj = JS_GetGlobalObject(this->ctx);

    JS_SetPropertyStr(this->ctx, global_obj, "addEventListener", JS_NewCFunction(this->ctx, api_add_event_listener, "addEventListener", 2));

    JS_FreeValue(this->ctx, global_obj);
}

void Context::init_api_crypto()
{
    JSValue global_obj, crypto;

    global_obj = JS_GetGlobalObject(this->ctx);

    crypto = JS_NewObject(this->ctx);

    JS_SetPropertyStr(this->ctx, crypto, "getRandomValues", JS_NewCFunction(ctx, api_crypto_get_random_values, "getRandomValues", 1));
    JS_SetPropertyStr(this->ctx, crypto, "randomUUID", JS_NewCFunction(ctx, api_crypto_random_uuid, "randomUUID", 0));

    JS_SetPropertyStr(this->ctx, global_obj, "crypto", crypto);

    JS_FreeValue(this->ctx, global_obj);
}

void Context::init_api_timeout()
{
    JSValue global_obj = JS_GetGlobalObject(this->ctx);

    JS_SetPropertyStr(this->ctx, global_obj, "setTimeout", JS_NewCFunction(this->ctx, api_set_timeout, "setTimeout", 2));
    JS_SetPropertyStr(this->ctx, global_obj, "clearTimeout", JS_NewCFunction(this->ctx, api_clear_timeout, "clearTimeout", 1));
    JS_SetPropertyStr(this->ctx, global_obj, "setInterval", JS_NewCFunction(this->ctx, api_set_interval, "setInterval", 1));
    JS_SetPropertyStr(this->ctx, global_obj, "clearInterval", JS_NewCFunction(this->ctx, api_clear_timeout, "clearInterval", 1));

    JS_FreeValue(this->ctx, global_obj);
}



