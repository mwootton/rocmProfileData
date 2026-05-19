import json
import dash
from dash import html
import dash_ag_grid as dag

from rpd_dash.util import db

dash.register_page(__name__, path="/tl/torch-ops", name="Torch Ops")

TORCH_OPS_SQL = """
SELECT apiName, category, count(*) as calls,
       sum(end - start) / 1000 as cpu_time_us,
       avg(end - start) / 1000 as avg_cpu_us
FROM api
WHERE domain = 'torch'
GROUP BY apiName, category
ORDER BY cpu_time_us DESC
"""

TORCH_SHAPES_SQL = """
SELECT apiName, category, args,
       (end - start) / 1000 as cpu_time_us
FROM api
WHERE domain = 'torch' AND category IN ('function', 'backward_function')
AND apiName LIKE 'aten::%'
"""


def _extract_shapes(args_str):
    try:
        data = json.loads(args_str)
        sizes = data.get("sizes", [])
        non_empty = [s for s in sizes if s]
        if non_empty:
            return str(non_empty)
    except (json.JSONDecodeError, TypeError):
        pass
    return ""


def layout():
    if not db.rpd_path:
        return html.Div("No RPD file loaded.")

    if not db.has_torch_ops():
        return html.Div([html.H2("Torch Ops"), html.P("No PyTorch annotations in this trace.")])

    try:
        fmt_num = {"function": "d3.format(',')(params.value)"}
        sections = [html.H2("Torch Ops Summary")]

        ops_df = db.query_df(TORCH_OPS_SQL)

        fwd_df = ops_df[ops_df["category"] == "function"].copy()
        bwd_df = ops_df[ops_df["category"] == "backward_function"].copy()

        if not fwd_df.empty:
            sections.append(html.H3("Forward Ops"))
            sections.append(dag.AgGrid(
                rowData=fwd_df.to_dict("records"),
                columnDefs=[
                    {"field": "apiName", "headerName": "Op", "flex": 3},
                    {"field": "calls", "headerName": "Calls", "flex": 1, "valueFormatter": fmt_num},
                    {"field": "cpu_time_us", "headerName": "CPU Time (us)", "flex": 1, "valueFormatter": fmt_num},
                    {"field": "avg_cpu_us", "headerName": "Avg CPU (us)", "flex": 1,
                     "valueFormatter": {"function": "d3.format(',.1f')(params.value)"}},
                ],
                defaultColDef={"sortable": True, "resizable": True, "filter": True},
                style={"height": "400px"},
            ))

        if not bwd_df.empty:
            sections.append(html.H3("Backward Ops", style={"marginTop": "25px"}))
            sections.append(dag.AgGrid(
                rowData=bwd_df.to_dict("records"),
                columnDefs=[
                    {"field": "apiName", "headerName": "Op", "flex": 3},
                    {"field": "calls", "headerName": "Calls", "flex": 1, "valueFormatter": fmt_num},
                    {"field": "cpu_time_us", "headerName": "CPU Time (us)", "flex": 1, "valueFormatter": fmt_num},
                    {"field": "avg_cpu_us", "headerName": "Avg CPU (us)", "flex": 1,
                     "valueFormatter": {"function": "d3.format(',.1f')(params.value)"}},
                ],
                defaultColDef={"sortable": True, "resizable": True, "filter": True},
                style={"height": "400px"},
            ))

        # Unique args / shapes view
        shapes_df = db.query_df(TORCH_SHAPES_SQL)
        if not shapes_df.empty:
            shapes_df["shapes"] = shapes_df["args"].apply(_extract_shapes)
            shaped = shapes_df[shapes_df["shapes"] != ""]
            if not shaped.empty:
                grouped = shaped.groupby(["apiName", "category", "shapes"]).agg(
                    calls=("cpu_time_us", "count"),
                    total_cpu_us=("cpu_time_us", "sum"),
                    avg_cpu_us=("cpu_time_us", "mean"),
                ).reset_index().sort_values("total_cpu_us", ascending=False)

                sections.append(html.H3("Ops by Input Shape", style={"marginTop": "25px"}))
                sections.append(dag.AgGrid(
                    rowData=grouped.to_dict("records"),
                    columnDefs=[
                        {"field": "apiName", "headerName": "Op", "flex": 2},
                        {"field": "category", "headerName": "Fwd/Bwd", "flex": 1},
                        {"field": "shapes", "headerName": "Input Shapes", "flex": 3, "tooltipField": "shapes"},
                        {"field": "calls", "headerName": "Calls", "flex": 1, "valueFormatter": fmt_num},
                        {"field": "total_cpu_us", "headerName": "Total CPU (us)", "flex": 1, "valueFormatter": fmt_num},
                        {"field": "avg_cpu_us", "headerName": "Avg CPU (us)", "flex": 1,
                         "valueFormatter": {"function": "d3.format(',.1f')(params.value)"}},
                    ],
                    defaultColDef={"sortable": True, "resizable": True, "filter": True},
                    style={"height": "500px"},
                ))

        return html.Div(sections)
    except Exception as e:
        return html.Div(f"Error loading torch ops: {e}")
