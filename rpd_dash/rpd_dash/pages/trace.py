import dash
from dash import html

from rpd_dash.util import db

dash.register_page(__name__, path="/trace", name="Timeline")


def layout():
    if not db.rpd_path:
        return html.Div("No RPD file loaded.")

    return html.Div([
        html.H2("Timeline Trace"),
        html.P("Generate Chrome Tracing JSON and view in Perfetto or download."),
        html.Div(
            [
                html.Button("Open in Perfetto", id="btn-perfetto",
                             style={"marginRight": "10px", "padding": "10px 20px", "fontSize": "14px"}),
                html.A("Download JSON", href="/tracedata", download="trace.json",
                       style={"padding": "10px 20px", "fontSize": "14px", "textDecoration": "none",
                              "border": "1px solid #ccc", "borderRadius": "4px", "color": "#333"}),
            ],
            style={"margin": "20px 0"},
        ),
        html.Pre(id="trace-log", style={"border": "1px solid #eee", "padding": "10px",
                                         "fontFamily": "monospace", "fontSize": "12px",
                                         "minHeight": "60px", "marginTop": "15px"}),
        html.Script("""
        (function() {
            const ORIGIN = 'https://ui.perfetto.dev';
            const btn = document.getElementById('btn-perfetto');
            const log = document.getElementById('trace-log');
            if (!btn) return;

            btn.addEventListener('click', async function() {
                log.textContent = 'Fetching trace JSON...\\n';
                try {
                    const resp = await fetch('/tracedata');
                    const blob = await resp.blob();
                    log.textContent += 'Buffering (' + (blob.size / 1e6).toFixed(1) + ' MB)...\\n';
                    const arrayBuffer = await blob.arrayBuffer();

                    log.textContent += 'Opening ui.perfetto.dev...\\n';
                    const win = window.open(ORIGIN);
                    if (!win) {
                        log.textContent += 'Popup blocked! Please allow popups for this site.\\n';
                        return;
                    }

                    const timer = setInterval(() => win.postMessage('PING', ORIGIN), 50);
                    window.addEventListener('message', function handler(evt) {
                        if (evt.data !== 'PONG') return;
                        clearInterval(timer);
                        window.removeEventListener('message', handler);
                        win.postMessage({
                            perfetto: {
                                buffer: arrayBuffer,
                                title: 'RPD Trace',
                            }
                        }, ORIGIN);
                        log.textContent += 'Trace sent to Perfetto.\\n';
                    });
                } catch(e) {
                    log.textContent += 'Error: ' + e.message + '\\n';
                }
            });
        })();
        """),
    ])
