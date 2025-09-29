import { create } from 'zustand';
import { DOT_COUNT } from '../lib/constants.js';
import { buildWsUrl, buildHttpBase } from '../lib/host.js';

const defaultHost = (() => {
  if (typeof window === 'undefined' || !window.location) {
    return '192.168.1.133:80';
  }
  const { hostname, host } = window.location;
  if (!host) {
    return '192.168.1.133:80';
  }
  if (hostname === 'localhost' || hostname === '127.0.0.1') {
    return '192.168.1.133:80';
  }
  return host;
})();

const createEmptyFrame = (fillValue = 0) => {
  const buffer = new Uint8Array(DOT_COUNT);
  if (fillValue) {
    buffer.fill(fillValue);
  }
  return buffer;
};

let socketRef = null;
let sendTimer = null;

export const useConnectionStore = create((set, get) => {
  const scheduleSend = () => {
    if (!socketRef || socketRef.readyState !== WebSocket.OPEN) {
      return;
    }
    if (sendTimer) {
      return;
    }
    sendTimer = setTimeout(() => {
      sendTimer = null;
      get().sendFramebuffer();
    }, 60);
  };

  const cleanupSocket = () => {
    if (socketRef) {
      socketRef.close(1000, 'client reset');
      socketRef = null;
    }
  };

  const pushEvent = (type, message) => {
    set((state) => ({
      events: [{ type, message }, ...state.events].slice(0, 6),
    }));
  };

  return {
    host: defaultHost,
    isConnecting: false,
    isConnected: false,
    lastError: null,
    httpBase: buildHttpBase(defaultHost),
    framebuffer: createEmptyFrame(),
    lastSentAt: null,
    events: [],
    invert: false,

    setHost: (host) => {
      set({ host, httpBase: buildHttpBase(host) });
    },

    connect: () => {
      const { host } = get();
      const url = buildWsUrl(host);

      if (!url) {
        set({ lastError: 'Enter the flip-dot IP address first.' });
        return;
      }

      cleanupSocket();

      try {
        const ws = new WebSocket(url);
        ws.binaryType = 'arraybuffer';
        socketRef = ws;

        set({
          isConnecting: true,
          lastError: null,
        });
        pushEvent('info', `Connecting to ${url}`);

        ws.onopen = () => {
          if (socketRef !== ws) {
            return;
          }
          set({
            isConnected: true,
            isConnecting: false,
          });
          pushEvent('success', 'WebSocket connected');
          get().sendFramebuffer();
        };

        ws.onclose = (event) => {
          if (socketRef === ws) {
            socketRef = null;
          }
          set({
            isConnected: false,
            isConnecting: false,
          });
          pushEvent('info', `WebSocket closed (${event.code})`);
        };

        ws.onerror = () => {
          set({
            lastError: 'WebSocket encountered an error',
          });
          pushEvent('error', 'WebSocket error');
        };

        ws.onmessage = (event) => {
          if (!(event.data instanceof ArrayBuffer)) {
            return;
          }
          const incoming = new Uint8Array(event.data);
          if (incoming.length !== DOT_COUNT) {
            return;
          }
          set({ framebuffer: new Uint8Array(incoming) });
        };
      } catch (error) {
        set({
          isConnecting: false,
          lastError: error.message || 'Failed to create WebSocket',
        });
        pushEvent('error', error.message || 'WebSocket failed');
      }
    },

    disconnect: () => {
      cleanupSocket();
      set({
        isConnected: false,
        isConnecting: false,
      });
      pushEvent('info', 'Disconnected');
    },

    updatePixel: (index, value) => {
      set((state) => {
        const nextValue = value ? 255 : 0;
        if (state.framebuffer[index] === nextValue) {
          return {};
        }
        const next = state.framebuffer.slice();
        next[index] = nextValue;
        return { framebuffer: next };
      });
      scheduleSend();
    },

    setFramebuffer: (nextFrame) => {
      set({ framebuffer: new Uint8Array(nextFrame) });
      scheduleSend();
    },

    clearFramebuffer: () => {
      set({ framebuffer: createEmptyFrame() });
      pushEvent('info', 'Framebuffer cleared');
      scheduleSend();
    },

    fillFramebuffer: () => {
      set({ framebuffer: createEmptyFrame(255) });
      pushEvent('info', 'Framebuffer filled');
      scheduleSend();
    },

    sendFramebuffer: () => {
      if (!socketRef || socketRef.readyState !== WebSocket.OPEN) {
        return;
      }
      const payload = get().framebuffer;
      if (payload.length !== DOT_COUNT) {
        pushEvent('error', `Unexpected framebuffer size: ${payload.length}`);
        return;
      }
      const frame = new Uint8Array(payload);
      socketRef.send(frame);
      set({ lastSentAt: Date.now() });
      pushEvent('success', 'Framebuffer pushed');
    },

    setInvert: (value) => set({ invert: value }),
    pushEvent,
  };
});
