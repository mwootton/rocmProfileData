import dash
from dash import html
import dash_ag_grid as dag

from rpd_dash.util import db

dash.register_page(__name__, path="/api", name="API Calls")

API_SQL = """
SELECT A.string as apiName, count(*) as total_calls
FROM rocpd_api
INNER JOIN rocpd_string A ON A.id = rocpd_api.apiName_id
GROUP BY apiName
ORDER BY total_calls DESC
"""


def layout():
    if not db.rpd_path:
        return html.Div("No RPD file loaded.")

    try:
        if db.table_exists("api"):
            df = db.query_df("SELECT apiName, count(*) as total_calls FROM api GROUP BY apiName ORDER BY total_calls DESC")
        else:
            df = db.query_df(API_SQL)

        return html.Div([
            html.H2("API Call Summary"),
            dag.AgGrid(
                rowData=df.to_dict("records"),
                columnDefs=[
                    {"field": "apiName", "headerName": "API Name", "flex": 3},
                    {"field": "total_calls", "headerName": "Total Calls", "flex": 1, "valueFormatter": {"function": "d3.format(',')(params.value)"}},
                ],
                defaultColDef={"sortable": True, "resizable": True, "filter": True},
                style={"height": "600px"},
            ),
        ])
    except Exception as e:
        return html.Div(f"Error loading API summary: {e}")
