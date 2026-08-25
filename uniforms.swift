// On-demand: emit the GameplayKit nextUniform() bit-pattern sequence for ONE seed.
//
// Blackjack's deck reconstruction (generate_win_easy_deck) consumes this uniform
// stream. The master seed is passed via the SEED env var; we print `count`
// 32-bit float bit patterns (one per line) so Python can decode them exactly.

import Foundation
import GameplayKit

guard let seedStr = ProcessInfo.processInfo.environment["SEED"],
      let seed = UInt64(seedStr.trimmingCharacters(in: .whitespaces)) else {
    FileHandle.standardError.write("SEED env var missing or not an integer\n".data(using: .utf8)!)
    exit(1)
}
let count = Int(ProcessInfo.processInfo.environment["COUNT"] ?? "512") ?? 512

let rng = GKMersenneTwisterRandomSource(seed: seed)
var bits: [String] = []
for _ in 0..<count {
    bits.append(String(rng.nextUniform().bitPattern))
}

let payload: [String: Any] = ["seed": seedStr, "count": count, "uniform_bits": bits]
let data = try JSONSerialization.data(withJSONObject: payload, options: [.sortedKeys])
try data.write(to: URL(fileURLWithPath: "uniforms.json"))
FileHandle.standardOutput.write(data)
FileHandle.standardOutput.write("\n".data(using: .utf8)!)
