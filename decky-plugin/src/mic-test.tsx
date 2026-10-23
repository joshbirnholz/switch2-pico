import { DialogButton, Focusable, ModalRoot, ProgressBar } from "@decky/ui";
import { useEffect, useRef, useState } from "react";
import { micTest, MicTestState } from "./api";

const words = (phase?: string): [string, string] => {
  switch (phase) {
    case "ready": return ["Record yourself using your microphone and then listen back.", "Once recording starts, speak into the mic."];
    case "preparing": return ["Preparing microphone", "The five-second recording will begin when the microphone is ready."];
    case "recording": return ["Recording...", "Speak toward the mic. (Recording for five seconds.)"];
    case "playback": return ["Now playing back...", "Make sure you can hear the recorded voice audio."];
    default: return ["Connecting to the microphone…", "Playback will come through your controller’s headphones at a reduced volume."];
  }
};
function AudioLevelIcon({ playback }: { playback: boolean }) {
  return <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor"
    strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true"
    style={{ flex: "none", color: "#a3adba" }}>
    {playback ? <>
      <path d="M3 9h4l5-4v14l-5-4H3z" />
      <path d="M16 8a6 6 0 0 1 0 8M19 5a10 10 0 0 1 0 14" />
    </> : <>
      <rect x="9" y="2" width="6" height="12" rx="3" />
      <path d="M5 10v2a7 7 0 0 0 14 0v-2M12 19v3M8 22h8" />
    </>}
  </svg>;
}

export function MicTestModal(props: { closeModal?(): void }) {
  const [state, setState] = useState<MicTestState>();
  const [error, setError] = useState("");
  const [busy, setBusy] = useState(false);
  const session = useRef(0);
  const mounted = useRef(true);
  const stopped = useRef(false);
  useEffect(() => {
    mounted.current = true;
    stopped.current = false;
    let alive = true;
    let timer: ReturnType<typeof setTimeout>;
    const poll = async () => {
      if (!alive || stopped.current) return;
      try {
        const r = await micTest("status", session.current);
        if (!alive || stopped.current) return;
        if (!r.ok || !r.test) throw new Error(r.error || "The microphone test could not be reached.");
        if (r.test.session !== session.current) throw new Error("This microphone test has closed. Reopen it to try again.");
        if (r.test.phase === "done") { props.closeModal?.(); return; }
        setState(r.test);
        if (!["done", "error", "idle"].includes(r.test.phase)) timer = setTimeout(poll, 120);
      } catch (e) {
        if (alive) setError(e instanceof Error ? e.message : String(e));
        if (session.current) void micTest("cancel", session.current).catch(() => {});
      }
    };
    void micTest("open", 0).then((r) => {
      if (!r.ok || !r.test) { if (alive) setError(r.error || "The microphone test could not start."); return; }
      if (!alive) { void micTest("cancel", r.test.session).catch(() => {}); return; }
      session.current = r.test.session;
      setState(r.test); timer = setTimeout(poll, 120);
    }).catch((e) => { if (alive) setError(String(e)); });
    return () => {
      alive = false;
      mounted.current = false;
      clearTimeout(timer);
      if (session.current) void micTest("cancel", session.current).catch(() => {});
    };
  }, []);
  const close = () => {
    if (session.current) void micTest("cancel", session.current).catch(() => {});
    props.closeModal?.();
  };
  const begin = async () => {
    if (busy) return;
    setBusy(true);
    try {
      const r = await micTest("record", session.current);
      if (!r.ok || !r.test) throw new Error(r.error || "Recording could not start.");
      if (mounted.current) setState(r.test);
    } catch (e) {
      stopped.current = true;
      if (mounted.current) setError(e instanceof Error ? e.message : String(e));
      void micTest("cancel", session.current).catch(() => {});
    }
    finally { if (mounted.current) setBusy(false); }
  };
  const phase = state?.phase;
  const [title, description] = words(phase);
  const failure = error || (phase === "error" ? state?.error : "");
  const level = state?.peak ? Math.max(0, Math.min(100, (20 * Math.log10(state.peak / 32768) + 60) / 60 * 100)) : 0;
  const playing = phase === "playback";
  return <ModalRoot closeModal={close}>
    <div style={{ fontSize: 22, fontWeight: 700, marginBottom: 16 }}>Test Microphone</div>
    <div style={{ fontWeight: 600, marginBottom: 8 }} aria-live="polite">
      {failure ? "Microphone test unavailable" : title}
    </div>
    <div style={{ fontSize: 14, lineHeight: 1.5, color: failure ? "#ffb4b4" : "#a3adba", marginBottom: 20 }}>
      {failure || description}
    </div>
    {!failure && <div style={{ display: "flex", alignItems: "center", gap: 12 }}>
      <AudioLevelIcon playback={playing} />
      <div style={{ flex: 1, minWidth: 0 }} role="meter" aria-label={playing ? "Playback audio level" : "Microphone audio level"}
        aria-valuemin={0} aria-valuemax={100} aria-valuenow={Math.round(level)}>
        <ProgressBar nProgress={level} nTransitionSec={0.1} />
      </div>
    </div>}
    <Focusable flow-children="row" style={{ display: "flex", gap: 12, marginTop: 24 }}>
      <DialogButton style={{ flex: 1 }} onClick={close}>
        {playing || failure ? "Close" : "Cancel"}
      </DialogButton>
      {phase === "ready" && !failure && <DialogButton disabled={busy} style={{ flex: 1 }} onClick={begin}>
        Begin Recording
      </DialogButton>}
    </Focusable>
  </ModalRoot>;
}
