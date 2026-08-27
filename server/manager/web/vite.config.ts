import { defineConfig } from "vite";
import preact from "@preact/preset-vite";

export default defineConfig({
  plugins: [preact()],
  server: {
    // Local dev against a running stack: `npm run dev` + proxy to nginx.
    proxy: {
      "/api": {
        target: "https://localhost:8444",
        changeOrigin: true,
        secure: false,
        ws: true,
        headers: { Host: "budka.budka.test" },
      },
    },
  },
});
