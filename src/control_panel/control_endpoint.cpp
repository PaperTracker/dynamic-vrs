#include "control_endpoint.hpp"
#include <windows.h>
#include <algorithm>
#include <iterator>
#include <tlhelp32.h>

namespace dvr::panel {
using namespace control;
namespace {
std::wstring mappingName(uint32_t processId) { return MappingPrefix + std::to_wstring(processId); }

bool headerValid(const SharedControl* shared) {
    return std::atomic_ref(shared->magic).load() == Magic && std::atomic_ref(shared->version).load() == Version &&
           std::atomic_ref(shared->size).load() == sizeof(SharedControl);
}

// A snapshot only fails while the layer is publishing, which takes microseconds.
bool readStatus(const SharedControl* shared, Status& status) {
    uint64_t sequence = 0;
    for (int attempt = 0; attempt < 64; ++attempt) {
        if (snapshot(shared->status, status, sequence)) return true;
        YieldProcessor();
    }
    return false;
}

bool liveEndpoint(uint32_t processId) {
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, mappingName(processId).c_str());
    if (!mapping) return false;
    bool live = false;
    if (auto* shared =
            static_cast<const SharedControl*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(SharedControl)))) {
        Status status;
        live = headerValid(shared) && readStatus(shared, status) && status.processId == processId &&
               status.result != static_cast<uint32_t>(Result::Closed);
        UnmapViewOfFile(shared);
    }
    CloseHandle(mapping);
    return live;
}
} // namespace

std::vector<Application> discoverApplications() {
    std::vector<Application> found;
    HANDLE processes = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (processes == INVALID_HANDLE_VALUE) return found;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(processes, &entry); more; more = Process32NextW(processes, &entry))
        if (entry.th32ProcessID && liveEndpoint(entry.th32ProcessID))
            found.push_back({entry.th32ProcessID, entry.szExeFile});
    CloseHandle(processes);
    return found;
}

void Patch::applyTo(Settings& settings) const {
    if (fields == AllFields) {
        settings = values;
        return;
    }
    if (fields & Mode) settings.mode = values.mode;
    if (fields & RingDegrees)
        std::copy(std::begin(values.ringDegrees), std::end(values.ringDegrees), settings.ringDegrees);
    if (fields & DitherDegrees) settings.ditherDegrees = values.ditherDegrees;
    if (fields & UniformRate) settings.uniformRate = values.uniformRate;
    if (fields & Rings) settings.rings = values.rings;
    if (fields & BlurMip) settings.blurMip = values.blurMip;
}

void Patch::merge(const Patch& other) {
    other.applyTo(values);
    fields |= other.fields;
}

bool Endpoint::open(uint32_t processId) {
    close();
    const auto name = mappingName(processId);
    mapping_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name.c_str());
    if (mapping_)
        view_ = static_cast<SharedControl*>(
            MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(SharedControl)));
    Status status;
    if (!view_ || !headerValid(view_) || !readStatus(view_, status) || status.processId != processId ||
        status.result == static_cast<uint32_t>(Result::Closed)) {
        close();
        return false;
    }
    writer_ = CreateMutexW(nullptr, FALSE, (name + L".Writer").c_str());
    if (!writer_) {
        close();
        return false;
    }
    processId_ = processId;
    session_ = status.session;
    return true;
}

void Endpoint::close() {
    if (view_) UnmapViewOfFile(view_);
    if (mapping_) CloseHandle(mapping_);
    if (writer_) CloseHandle(writer_);
    view_ = nullptr;
    mapping_ = writer_ = nullptr;
    processId_ = 0;
    session_ = 0;
}

bool Endpoint::read(Status& status) const { return view_ && readStatus(view_, status); }

WriteResult Endpoint::write(const Patch& patch, uint64_t& sequence) {
    if (!view_) return WriteResult::Failed;
    const DWORD wait = WaitForSingleObject(writer_, 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return WriteResult::Busy;
    WriteResult result = WriteResult::Busy;
    Status status;
    if (readStatus(view_, status)) {
        Request request{status.session, status.accepted};
        Request previous;
        uint64_t previousSequence = 0;
        if (snapshot(view_->request, previous, previousSequence) && previousSequence > status.processedSequence &&
            previous.session == status.session)
            request.settings = previous.settings;
        patch.applyTo(request.settings);
        if (status.session != session_ || status.result == static_cast<uint32_t>(Result::Closed))
            result = WriteResult::Failed;
        else if (!valid(request.settings))
            result = WriteResult::Invalid;
        else {
            sequence = publish(view_->request, request);
            result = WriteResult::Sent;
        }
    }
    ReleaseMutex(writer_);
    return result;
}
} // namespace dvr::panel
