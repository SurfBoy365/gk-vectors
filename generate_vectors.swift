// Generates ground-truth vectors for Apple GameplayKit's Mersenne Twister RNG.
//
// This is the one piece that needs macOS: turning a
// master seed into the raw nextUniform() stream (and 3 derived sub-seeds). We run this on a free GitHub Actions macOS runner, capture
// bit-exact output, and use it to build + verify a pure-Python reimplementation
// that runs on Windows/Linux with no Apple hardware.
//
// Reads:  seeds.json  (a JSON array of seed values, given as strings)
// Writes: vectors.json (per-seed: uniform bit patterns, raw ints, 3 sub-seeds)

import Foundation
import GameplayKit

let N = 1300  // draws per seed (>2 full MT blocks, enough for state recovery)

// --- read seeds.json (array of strings to preserve full 64-bit precision) ---
let seedsData = try Data(contentsOf: URL(fileURLWithPath: "seeds.json"))
let rawSeeds = try JSONSerialization.jsonObject(with: seedsData) as! [Any]
let seedStrings = rawSeeds.map { String(describing: $0) }

var vectors: [[String: Any]] = []

for str in seedStrings {
    guard let seed = UInt64(str) else {
        FileHandle.standardError.write("skipping non-integer seed: \(str)\n".data(using: .utf8)!)
        continue
    }

    // 1) nextUniform() stream — the exact Float values produced.
    //    Store the 32-bit IEEE-754 bit pattern so the ground truth is exact.
    let ru = GKMersenneTwisterRandomSource(seed: seed)
    var uniformBits: [String] = []
    var uniformValues: [Double] = []
    for _ in 0..<N {
        let u = ru.nextUniform()
        uniformBits.append(String(u.bitPattern))
        uniformValues.append(Double(u))
    }

    // 2) nextInt() stream — the underlying integer sequence, to pin down which
    //    Mersenne Twister variant/seeding GameplayKit uses.
    let ri = GKMersenneTwisterRandomSource(seed: seed)
    var nextInts: [String] = []
    for _ in 0..<N { nextInts.append(String(ri.nextInt())) }

    // 3) Three derived sub-seeds: skip one nextUniform(), then run
    //    GetRandom(INT_MIN, INT_MAX) three times in 32-bit Float math.
    //    (Mirrors the seed-derivation routine in the Python solver.)
    let rg = GKMersenneTwisterRandomSource(seed: seed)
    _ = rg.nextUniform()  // skip = 1
    let lo = Int32(bitPattern: 0x80000000)  // INT_MIN
    let hi = Int32(bitPattern: 0x7FFFFFFF)  // INT_MAX
    let range = Float(Int64(hi) - Int64(lo))
    let loF = Float(lo)
    let hiF = Float(hi)  // rounds to 2^31; used as the reject edge
    var subSeeds: [Int] = []
    for _ in 0..<3 {
        var result: Int32 = 0
        repeat {
            let u = rg.nextUniform()
            let t = u * range + loF
            if !t.isFinite {
                result = lo
            } else if t >= hiF {
                result = hi          // triggers the reject-and-redraw below
            } else {
                result = Int32(t)    // safe: lo <= t < 2^31
            }
        } while result == hi
        subSeeds.append(Int(result))
    }

    vectors.append([
        "seed": str,
        "uniform_bits": uniformBits,
        "uniform_values": uniformValues,
        "next_int": nextInts,
        "get_random_seeds": subSeeds,
    ])
}

let payload: [String: Any] = [
    "generator": "GKMersenneTwisterRandomSource",
    "n_draws": N,
    "count": vectors.count,
    "vectors": vectors,
]
let out = try JSONSerialization.data(withJSONObject: payload,
                                     options: [.prettyPrinted, .sortedKeys])
try out.write(to: URL(fileURLWithPath: "vectors.json"))
FileHandle.standardOutput.write(out)
FileHandle.standardOutput.write("\n".data(using: .utf8)!)
