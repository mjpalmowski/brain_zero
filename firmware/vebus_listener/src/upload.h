// Writing a settings file (.rvsc) to the Multi, from the Multi settings page. The file is parsed and
// shown against the latest read (rvsc.h). On confirmation the job reads the Multi afresh, checks the
// file against that read, writes what differs (requester.h, start_write(): the changed settings, and,
// if the assistants differ, the assistant records and a reset, as mk2vsc writes them), then reads the
// Multi again and requires it to hold the file. Plain C++, so test/upload_test.cpp can run it on a PC;
// main.cpp calls step() with the requester's lock held.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "requester.h"
#include "rvsc.h"

namespace upload {

enum State : uint8_t { EMPTY, LOADED, READING, WRITING, VERIFYING, DONE, FAILED };

class Uploader {
  public:
    // Parses a file; false with the reason if it cannot be written to any Multi. A job must not run.
    bool load(const uint8_t *data, size_t n, const char *name, char *why, size_t cap);
    void clear();  // forgets the file, unless a job runs
    // Starts the job: a fresh read first. False with *why if no file is loaded, a job runs, or the
    // read cannot start (the controller off, a test run, another read or a write running).
    bool start(vebus::Requester &r, int64_t now, const char **why, bool access_only = false);
    bool access_only() const { return access_only_; }
    // Before the writes begin, the job can be called off; the read in flight finishes on its own.
    bool cancel(const char **why);
    void step(vebus::Requester &r, int64_t now, int64_t utc = 0);

    State state() const { return state_; }
    static const char *state_name(State s);
    bool busy() const { return state_ == READING || state_ == WRITING || state_ == VERIFYING; }
    const rvsc::File &file() const { return file_; }
    const char *name() const { return name_; }
    size_t bytes() const { return bytes_; }
    const char *message() const { return message_; }  // how the last job went
    const rvsc::Check &job_check() const { return check_; }  // the check the job wrote by
    const rvsc::Check &preview(const vebus::Requester::SettingsRead &read, int64_t utc);
    int64_t started() const { return started_; }

  private:
    void finish(State s, const char *fmt, ...);

    rvsc::File file_;
    rvsc::Check check_;
    rvsc::Check comparison_;
    State state_ = EMPTY;
    bool access_only_ = false;
    char name_[48] = "";
    char message_[200] = "";
    size_t bytes_ = 0;
    int64_t started_ = -1, read_started_ = -1;
};

}  // namespace upload
