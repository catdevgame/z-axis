import CoreGraphics
import Foundation
// Prints the CGWindowID of the largest on-screen window owned by the given pid.
let pid = Int32(CommandLine.arguments[1])!
let list = CGWindowListCopyWindowInfo([.optionAll], kCGNullWindowID) as! [[String: Any]]
var best: (Int, Double) = (0, 0)
for w in list {
  guard (w[kCGWindowOwnerPID as String] as? Int32) == pid,
        let b = w[kCGWindowBounds as String] as? [String: Double],
        let id = w[kCGWindowNumber as String] as? Int else { continue }
  let area = (b["Width"] ?? 0) * (b["Height"] ?? 0)
  if area > best.1 { best = (id, area) }
}
if best.0 != 0 { print(best.0) }
