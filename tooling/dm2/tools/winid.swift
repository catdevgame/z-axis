import CoreGraphics
import Foundation
// Prints the CGWindowID of the largest on-screen, layer-0 window owned by the
// given pid (falls back to the largest window of any kind).
let pid = Int32(CommandLine.arguments[1])!
let list = CGWindowListCopyWindowInfo([.optionAll], kCGNullWindowID) as! [[String: Any]]
var best: (Int, Double) = (0, 0)
var any: (Int, Double) = (0, 0)
for w in list {
  guard (w[kCGWindowOwnerPID as String] as? Int32) == pid,
        let b = w[kCGWindowBounds as String] as? [String: Double],
        let id = w[kCGWindowNumber as String] as? Int else { continue }
  let area = (b["Width"] ?? 0) * (b["Height"] ?? 0)
  let onscreen = (w[kCGWindowIsOnscreen as String] as? Bool) ?? false
  let layer = (w[kCGWindowLayer as String] as? Int) ?? 0
  if onscreen && layer == 0 && area > best.1 { best = (id, area) }
  if area > any.1 { any = (id, area) }
}
let pick = best.0 != 0 ? best.0 : any.0
if pick != 0 { print(pick) }
