import { render } from "preact";
import { App } from "./app";
import { connectWs, refreshAuth, refreshDevices } from "./store";
import "./styles.css";

refreshAuth()
  .then((st) => {
    if (st.logged_in || st.mode === "off") {
      refreshDevices().catch(() => undefined);
      connectWs();
    }
  })
  .catch(() => undefined);
render(<App />, document.getElementById("app")!);
