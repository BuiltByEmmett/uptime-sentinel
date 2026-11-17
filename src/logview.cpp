#include "logview.h"

#include "monitor.h"
#include "storage.h"
#include "timekeeper.h"

namespace logview {

size_t count() {
  Cause cause;
  uint32_t startMono;
  return storage::count() + (monitor::activeEvent(cause, startMono) ? 1 : 0);
}

Row get(size_t index) {
  Row row;
  Cause cause;
  uint32_t startMono;
  if (monitor::activeEvent(cause, startMono)) {
    if (index == 0) {
      row.start = timekeeper::valid() ? timekeeper::toEpoch(startMono) : 0;
      row.duration = timekeeper::mono() - startMono;
      row.cause = cause;
      row.active = true;
      return row;
    }
    index--;
  }
  const storage::OutageRecord &r = storage::get(index);
  row.start = r.start;
  row.duration = r.duration;
  row.cause = static_cast<Cause>(r.cause);
  row.active = false;
  return row;
}

}  // namespace logview
