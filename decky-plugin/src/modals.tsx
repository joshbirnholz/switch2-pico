// Pop-up lists (profile and button pickers), paired controllers and the
// firmware update.
import { addEventListener, removeEventListener } from "@decky/api";
import { ConfirmModal, DialogButton, Focusable, ModalRoot, ProgressBar, showModal } from "@decky/ui";
import { ReactNode, useEffect, useState } from "react";
import { action, getStatus, installUpdate } from "./api";
import { PIDS, Status } from "./model";

const row: React.CSSProperties = {
  display: "flex",
  alignItems: "center",
  gap: 12,
  width: "100%",
  minHeight: 52,
  padding: "8px 12px",
  textAlign: "left",
  boxSizing: "border-box",
};
const iconBox: React.CSSProperties = { flex: "none", minWidth: 40, display: "flex", justifyContent: "center" };
const sub: React.CSSProperties = { fontSize: 13, color: "#a3adba", marginTop: 2, lineHeight: 1.3 };

// Buttons with a translucent focus highlight, as in Steam's own lists (a
// solid white one would hide white button icons).
export const SOFT_CSS = `
.s2p-soft { background: rgba(255,255,255,0.07); border-radius: 4px; color: #e6e9ed; cursor: pointer; }
.s2p-soft.s2p-sel { background: rgba(26,159,255,0.22); }
.s2p-soft.s2p-focus { background: rgba(255,255,255,0.20); }
`;
export function SoftButton(props: { onActivate(): void; style?: React.CSSProperties; selected?: boolean; children: ReactNode }) {
  return (
    <Focusable
      className={"s2p-soft" + (props.selected ? " s2p-sel" : "")}
      focusClassName="s2p-focus"
      style={props.style}
      onActivate={() => props.onActivate()}
      onClick={() => props.onActivate()}
    >
      {props.children}
    </Focusable>
  );
}

export interface PickItem {
  key: number;
  icon: ReactNode;
  title: string;
  desc?: string;
  selected?: boolean;
}

// A list to pick one item from, each with an icon and a description line.
export function PickerModal(props: {
  title: string;
  subtitle?: string;
  items: PickItem[];
  footer?: string;
  onPick(key: number): void;
  closeModal?(): void;
}) {
  return (
    <ModalRoot closeModal={props.closeModal}>
      <style>{SOFT_CSS}</style>
      <div style={{ fontSize: 22, fontWeight: 700 }}>{props.title}</div>
      {props.subtitle && <div style={{ ...sub, marginBottom: 12 }}>{props.subtitle}</div>}
      <Focusable flow-children="column" style={{ display: "flex", flexDirection: "column", gap: 4, maxHeight: "60vh", overflowY: "auto" }}>
        {props.items.map((it) => (
          <SoftButton
            key={it.key}
            style={row}
            selected={it.selected}
            onActivate={() => {
              props.onPick(it.key);
              props.closeModal?.();
            }}
          >
            <span style={iconBox}>{it.icon}</span>
            <span style={{ flex: 1, minWidth: 0 }}>
              <div style={{ fontWeight: 600 }}>{it.title}</div>
              {it.desc && <div style={sub}>{it.desc}</div>}
            </span>
            {it.selected && (
              <svg width="18" height="18" viewBox="0 0 18 18" aria-hidden="true">
                <path d="M4 9.5l3.2 3L14 5.5" fill="none" stroke="#ffffff" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round" />
              </svg>
            )}
          </SoftButton>
        ))}
      </Focusable>
      {props.footer && <div style={{ ...sub, marginTop: 12 }}>{props.footer}</div>}
    </ModalRoot>
  );
}

export function confirm(title: string, description: string, ok: string, onOK: () => void, destructive = false) {
  showModal(
    <ConfirmModal strTitle={title} strDescription={description} strOKButtonText={ok} bDestructiveWarning={destructive} onOK={onOK} />,
  );
}

// Remembered controllers, each with Forget, and Forget all.
export function PairedModal(props: { closeModal?(): void; onChange(): void }) {
  const [st, setSt] = useState<Status | null>(null);
  const reload = async () => {
    const r = await getStatus();
    if (r.ok && r.status) setSt(r.status);
  };
  useEffect(() => {
    reload();
  }, []);
  const forget = (addr: string | null) =>
    confirm(
      addr ? "Forget this controller?" : "Forget all controllers?",
      "It still remembers the dongle but won't reconnect until you pair it again (hold its Sync button while pairing).",
      addr ? "Forget" : "Forget all",
      async () => {
        await action(addr ? "forget&addr=" + addr : "forget");
        await reload();
        props.onChange();
      },
      true,
    );
  const list = st?.bond.list ?? [];
  const connected = new Set((st?.links ?? []).filter((k) => k.state === "ready").map((k) => k.addr));
  return (
    <ModalRoot closeModal={props.closeModal}>
      <div style={{ display: "flex", alignItems: "center", justifyContent: "space-between", gap: 12, marginBottom: 12 }}>
        <div style={{ fontSize: 22, fontWeight: 700 }}>Paired controllers</div>
        <DialogButton style={{ width: "auto", minWidth: 0, padding: "0 16px", height: 34 }} onClick={() => props.closeModal?.()}>
          Back
        </DialogButton>
      </div>
      <Focusable style={{ display: "flex", flexDirection: "column", gap: 4 }}>
        {!list.length && <div style={sub}>None yet.</div>}
        {list.map((b) => (
          <Focusable key={b.addr} style={{ ...row, justifyContent: "space-between", background: "#1f252e", borderRadius: 4 }}>
            <span>
              <div style={{ fontWeight: 600, display: "flex", alignItems: "center", gap: 8 }}>
                {PIDS[b.pid] || "Controller"}
                {connected.has(b.addr) && (
                  <span style={{ fontSize: 11, fontWeight: 700, padding: "2px 6px", borderRadius: 3, background: "#173b25", color: "#7fe0a2" }}>
                    Connected
                  </span>
                )}
              </div>
              <div style={sub}>{b.addr}</div>
            </span>
            <DialogButton style={{ width: "auto", minWidth: 0, padding: "0 16px", height: 34 }} onClick={() => forget(b.addr)}>
              Forget
            </DialogButton>
          </Focusable>
        ))}
        {list.length > 0 && (
          <DialogButton style={{ marginTop: 8 }} onClick={() => forget(null)}>
            Forget all…
          </DialogButton>
        )}
      </Focusable>
    </ModalRoot>
  );
}

const STAGES: Record<string, string> = {
  download: "Downloading from GitHub…",
  check: "Checking the image…",
  write: "Writing to the dongle…",
  restart: "Restarting the dongle…",
};

// Downloads and installs the latest firmware, showing its progress.
export function UpdateModal(props: { version: string; closeModal?(): void; onDone(): void }) {
  const [stage, setStage] = useState("download");
  const [pct, setPct] = useState(0);
  const [result, setResult] = useState<{ ok: boolean; text: string } | null>(null);
  useEffect(() => {
    const listener = addEventListener<[string, number]>("fw_progress", (s, p) => {
      setStage(s);
      setPct(p);
    });
    installUpdate().then((r) => {
      setResult(
        r.ok
          ? { ok: true, text: `Installed ${r.version ?? props.version}. The dongle will restart, and then you'll need to reconnect your controller.` }
          : { ok: false, text: `Update failed: ${r.error}. The dongle still has its current firmware, so you can try again.` },
      );
      props.onDone();
    });
    return () => removeEventListener("fw_progress", listener);
  }, []);
  return (
    <ModalRoot closeModal={props.closeModal} bDisableBackgroundDismiss={!result} bHideCloseIcon={!result}>
      <div style={{ fontSize: 22, fontWeight: 700, marginBottom: 12 }}>Updating to {props.version}</div>
      {!result && (
        <>
          <div style={{ textAlign: "center", marginBottom: 8 }}>
            {STAGES[stage] ?? stage}
            {stage === "write" ? ` ${pct}%` : ""}
          </div>
          <ProgressBar
            nProgress={stage === "write" ? pct : stage === "restart" ? 100 : undefined}
            indeterminate={stage === "download" || stage === "check"}
          />
          <div style={{ ...sub, marginTop: 12, textAlign: "center" }}>
            Keep the dongle plugged in. If the update stops partway, the dongle will keep its current firmware.
          </div>
        </>
      )}
      {result && (
        <>
          <div style={{ color: result.ok ? "#e6e9ed" : "#ffb4b4" }}>{result.text}</div>
          <DialogButton style={{ marginTop: 16 }} onClick={() => props.closeModal?.()}>
            Close
          </DialogButton>
        </>
      )}
    </ModalRoot>
  );
}
