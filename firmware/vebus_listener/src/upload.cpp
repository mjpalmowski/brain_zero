#include "upload.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace upload {

const char *Uploader::state_name(State s) {
    static const char *const NAME[] = {"empty", "loaded", "reading", "writing", "verifying", "done", "failed"};
    return s <= FAILED ? NAME[s] : "?";
}

void Uploader::finish(State s, const char *fmt, ...) {
    check_.plan.access.erase_bytes();
    state_ = s;
    va_list a;
    va_start(a, fmt);
    vsnprintf(message_, sizeof message_, fmt, a);
    va_end(a);
}

bool Uploader::load(const uint8_t *data, size_t n, const char *name, char *why, size_t cap) {
    if (busy()) {
        snprintf(why, cap, "a file is being written: wait for it to finish");
        return false;
    }
    state_ = EMPTY;
    access_only_ = false;
    message_[0] = 0;
    check_.plan.access.clear();
    if (!rvsc::parse(data, n, file_, why, cap)) { file_.credentials.clear(); return false; }
    snprintf(name_, sizeof name_, "%s", name ? name : "");
    bytes_ = n;
    state_ = LOADED;
    return true;
}

void Uploader::clear() {
    if (busy()) return;
    state_ = EMPTY;
    access_only_ = false;
    name_[0] = message_[0] = 0;
    bytes_ = 0;
    file_.credentials.clear();
    check_.plan.access.clear();
    comparison_.plan.access.clear();
    memset(static_cast<void *>(&file_), 0, sizeof file_);
}

const rvsc::Check &Uploader::preview(const vebus::Requester::SettingsRead &read, int64_t utc) {
    rvsc::check(file_, read, comparison_, utc);
    comparison_.plan.access.erase_bytes();
    return comparison_;
}

bool Uploader::start(vebus::Requester &r, int64_t now, const char **why, bool access_only) {
    const char *w = state_ == EMPTY ? "no file loaded" : busy() ? "a file is already being written" : nullptr;
    if (!w && access_only && !file_.credentials.count) w = "no embedded credentials to test";
    if (!w && !r.read_settings(now)) w = "the Multi cannot be read now (the controller off, a test run, or a read or "
                                         "write running)";
    if (why) *why = w ? w : "";
    if (w) return false;
    started_ = read_started_ = now;
    access_only_ = access_only;
    state_ = READING;
    message_[0] = 0;
    return true;
}

bool Uploader::cancel(const char **why) {
    const char *w = state_ == READING ? nullptr
                    : busy()          ? "the Multi is being written: a write cannot be called off"
                                      : "nothing to call off";
    if (why) *why = w ? w : "";
    if (w) return false;
    finish(LOADED, "called off before anything was written");
    return true;
}

void Uploader::step(vebus::Requester &r, int64_t now, int64_t utc) {
    const vebus::Requester::SettingsRead &read = r.settings();
    bool read_done = !read.active && read.done && read.started == read_started_;
    switch (state_) {
    case READING: {
        if (!read_done) {
            if (!read.active && read.started != read_started_) finish(FAILED, "the read before writing did not run");
            return;
        }
        if (!rvsc::check(file_, read, check_, utc)) {
            finish(FAILED, "not written: %s", check_.why);
            return;
        }
        const vebus::Requester::WritePlan &p = check_.plan;
        if (access_only_) {
            const char *why = nullptr;
            bool ok = r.start_access_test(now, file_.credentials, &why, utc);
            check_.plan.access.erase_bytes();
            if (!ok) finish(FAILED, "credentials not tested: %s", why);
            else state_ = WRITING;
            return;
        }
        if (p.settings == 0 && !p.assistants) {
            finish(DONE, "nothing written: the Multi already holds this file");
            return;
        }
        const char *why = nullptr;
        if (!r.start_write(now, p, &why, utc)) {
            check_.plan.access.erase_bytes();
            finish(FAILED, "not written: %s", why);
            return;
        }
        check_.plan.access.erase_bytes();
        state_ = WRITING;
        return;
    }
    case WRITING: {
        const vebus::Requester::WriteJob &j = r.write_job();
        if (j.active || !j.done) return;
        if (j.failed) {
            finish(FAILED, "%s stopped: %s", access_only_ ? "credential test" : "writing", j.problem);
            return;
        }
        if (access_only_) {
            finish(DONE, "embedded credentials accepted (%u); no settings written and no reset sent", j.authorized);
            return;
        }
        if (!r.read_settings(now)) return;  // try again on the next step
        read_started_ = now;
        state_ = VERIFYING;
        return;
    }
    case VERIFYING: {
        if (!read_done) {
            if (!read.active && read.started != read_started_) finish(FAILED, "the read after writing did not run");
            return;
        }
        const rvsc::Check &after = preview(read, utc);
        const vebus::Requester::WritePlan &p = check_.plan;
        if (!after.ok) {
            finish(FAILED, "written, but the read afterwards does not match the file: %s", after.why);
        } else if (after.plan.settings || after.plan.assistants) {
            finish(FAILED, "written, but the Multi still differs from the file: %d setting%s%s", after.plan.settings,
                   after.plan.settings == 1 ? "" : "s", after.plan.assistants ? " and the assistants" : "");
        } else {
            const vebus::Requester::WriteJob &j = r.write_job();
            finish(DONE, "written and read back: %d setting%s%s, in %.0f s%s%s", p.settings, p.settings == 1 ? "" : "s",
                   p.assistants ? ", the assistants and a reset" : p.restart ? ", and a reset" : "", (now - started_) / 1e6, j.startup[0] ? "; " : "",
                   j.startup);
        }
        return;
    }
    default:
        return;
    }
}

}  // namespace upload
