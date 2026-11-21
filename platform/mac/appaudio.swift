// appaudio <pid> <seconds> <out.wav>: records the audio a process plays
// (ScreenCaptureKit, 48 kHz stereo) into a 16-bit WAV. SIGINT/SIGTERM stop
// the recording early and still write the file. Gaps between the capture's
// buffers (by presentation time) are filled with silence, so the file keeps
// the wall-clock timeline.
import Foundation
import ScreenCaptureKit
import CoreMedia
import AVFoundation

let args = CommandLine.arguments
guard args.count == 4, let pid = Int32(args[1]), let seconds = Double(args[2]) else {
    FileHandle.standardError.write("usage: appaudio <pid> <seconds> <out.wav>\n".data(using: .utf8)!); exit(2)
}
let outPath = args[3]

final class Recorder: NSObject, SCStreamOutput, SCStreamDelegate {
    var samples = [Int16]()
    let lock = NSLock()
    var firstPTS: Double = -1
    var lastEnd: Double = -1
    var gaps = 0
    var gapFrames = 0
    func stream(_ stream: SCStream, didOutputSampleBuffer sb: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .audio, sb.isValid else { return }
        let pts = CMSampleBufferGetPresentationTimeStamp(sb).seconds
        let frames = CMSampleBufferGetNumSamples(sb)
        // Ask for the list size first: planar stereo needs two buffers, more
        // than an AudioBufferList value holds.
        var size = 0
        CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sb, bufferListSizeNeededOut: &size, bufferListOut: nil, bufferListSize: 0, blockBufferAllocator: nil, blockBufferMemoryAllocator: nil, flags: 0, blockBufferOut: nil)
        guard size > 0 else { return }
        let raw = UnsafeMutableRawPointer.allocate(byteCount: size, alignment: 16)
        defer { raw.deallocate() }
        let list = raw.bindMemory(to: AudioBufferList.self, capacity: 1)
        var block: CMBlockBuffer?
        guard CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sb, bufferListSizeNeededOut: nil, bufferListOut: list, bufferListSize: size, blockBufferAllocator: nil, blockBufferMemoryAllocator: nil, flags: 0, blockBufferOut: &block) == noErr else { return }
        append(UnsafeMutableAudioBufferListPointer(list), frames: frames, pts: pts)
    }
    func append(_ buffers: UnsafeMutableAudioBufferListPointer, frames: Int, pts: Double) {
        lock.lock(); defer { lock.unlock() }
        if firstPTS < 0 { firstPTS = pts }
        if lastEnd >= 0 && abs(pts - lastEnd) > 0.002 {
            gaps += 1
            if pts > lastEnd {
                let missing = min(Int(((pts - lastEnd) * 48000.0).rounded()), 48000 * 5)
                samples.append(contentsOf: repeatElement(Int16(0), count: missing * 2))
                gapFrames += missing
            }
        }
        lastEnd = pts + Double(frames) / 48000.0
        func s(_ v: Float) -> Int16 { Int16(max(-32768, min(32767, (v * 32767).rounded()))) }
        if buffers.count >= 2 {
            let l = buffers[0].mData!.assumingMemoryBound(to: Float.self)
            let r = buffers[1].mData!.assumingMemoryBound(to: Float.self)
            for i in 0..<frames { samples.append(s(l[i])); samples.append(s(r[i])) }
        } else if buffers.count == 1 {
            let p = buffers[0].mData!.assumingMemoryBound(to: Float.self)
            let ch = Int(buffers[0].mNumberChannels)
            for i in 0..<frames { samples.append(s(p[i*ch])); samples.append(s(p[i*ch + (ch > 1 ? 1 : 0)])) }
        }
    }
    func stream(_ stream: SCStream, didStopWithError error: Error) {
        FileHandle.standardError.write("stream stopped: \(error)\n".data(using: .utf8)!)
    }
}

func writeWav(_ path: String, _ s: [Int16]) {
    var d = Data()
    func u32(_ v: UInt32) { var x = v.littleEndian; d.append(Data(bytes: &x, count: 4)) }
    func u16(_ v: UInt16) { var x = v.littleEndian; d.append(Data(bytes: &x, count: 2)) }
    let bytes = UInt32(s.count * 2)
    d.append("RIFF".data(using: .ascii)!); u32(36 + bytes); d.append("WAVEfmt ".data(using: .ascii)!)
    u32(16); u16(1); u16(2); u32(48000); u32(48000 * 4); u16(4); u16(16)
    d.append("data".data(using: .ascii)!); u32(bytes)
    s.withUnsafeBytes { d.append(contentsOf: $0) }
    try! d.write(to: URL(fileURLWithPath: path))
}

let rec = Recorder()
var signalSources = [DispatchSourceSignal]()
Task {
    do {
        let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: false)
        guard let app = content.applications.first(where: { $0.processID == pid }) else {
            FileHandle.standardError.write("no app with pid \(pid)\n".data(using: .utf8)!); exit(1)
        }
        guard let display = content.displays.first else { exit(1) }
        let filter = SCContentFilter(display: display, including: [app], exceptingWindows: [])
        let config = SCStreamConfiguration()
        config.capturesAudio = true
        config.sampleRate = 48000
        config.channelCount = 2
        config.excludesCurrentProcessAudio = true
        config.width = 2; config.height = 2
        config.minimumFrameInterval = CMTime(value: 1, timescale: 1)
        let stream = SCStream(filter: filter, configuration: config, delegate: rec)
        try stream.addStreamOutput(rec, type: .audio, sampleHandlerQueue: DispatchQueue(label: "audio"))
        try await stream.startCapture()
        FileHandle.standardError.write("recording \(app.applicationName) (\(pid)) for \(seconds) s\n".data(using: .utf8)!)
        let finish = {
            rec.lock.lock()
            writeWav(outPath, rec.samples)
            FileHandle.standardError.write("wrote \(rec.samples.count / 2) frames (\(Double(rec.samples.count / 2) / 48000.0) s), \(rec.gaps) timestamp gaps (\(rec.gapFrames) frames of silence filled)\n".data(using: .utf8)!)
            rec.lock.unlock()
            exit(0)
        }
        for sig in [SIGINT, SIGTERM] {
            signal(sig, SIG_IGN)
            let source = DispatchSource.makeSignalSource(signal: sig, queue: .main)
            source.setEventHandler { Task { try? await stream.stopCapture(); finish() } }
            source.resume()
            signalSources.append(source)
        }
        try await Task.sleep(nanoseconds: UInt64(seconds * 1e9))
        try await stream.stopCapture()
        finish()
    } catch {
        FileHandle.standardError.write("error: \(error)\n".data(using: .utf8)!); exit(1)
    }
}
RunLoop.main.run()
