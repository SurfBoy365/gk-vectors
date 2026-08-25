// On-demand: compute the 3 sub-seeds for ONE master seed.
//
// The master seed is passed in via the SEED environment variable (the workflow
// wires the workflow_dispatch input to it). Output: sub_seeds.json.
//
// Mirrors the seed-derivation routine in the Python solver: seed a
// GKMersenneTwisterRandomSource, skip one nextUniform(), then run
// GetRandom(INT_MIN, INT_MAX) three times in 32-bit Float math.

import Foundation
import GameplayKit

guard let seedStr = ProcessInfo.processInfo.environment["SEED"],
      let seed = UInt64(seedStr.trimmingCharacters(in: .whitespaces)) else {
    FileHandle.standardError.write("SEED env var missing or not an integer\n".data(using: .utf8)!)
    exit(1)
}

let rng = GKMersenneTwisterRandomSource(seed: seed)
_ = rng.nextUniform()  // skip = 1

let lo = Int32(bitPattern: 0x80000000)  // INT_MIN
let hi = Int32(bitPattern: 0x7FFFFFFF)  // INT_MAX
let range = Float(Int64(hi) - Int64(lo))
let loF = Float(lo)
let hiF = Float(hi)

var subSeeds: [Int] = []
for _ in 0..<3 {
    var result: Int32 = 0
    repeat {
        let u = rng.nextUniform()
        let t = u * range + loF
        if !t.isFinite {
            result = lo
        } else if t >= hiF {
            result = hi          // reject and redraw
        } else {
            result = Int32(t)
        }
    } while result == hi
    subSeeds.append(Int(result))
}

let payload: [String: Any] = ["seed": seedStr, "sub_seeds": subSeeds]
let data = try JSONSerialization.data(withJSONObject: payload, options: [.sortedKeys])
try data.write(to: URL(fileURLWithPath: "sub_seeds.json"))
FileHandle.standardOutput.write(data)
FileHandle.standardOutput.write("\n".data(using: .utf8)!)
