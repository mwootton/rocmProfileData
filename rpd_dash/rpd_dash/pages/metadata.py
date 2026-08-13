import dash
from dash import html
import dash_ag_grid as dag

from rpd_dash.util import db

dash.register_page(__name__, path="/metadata", name="Metadata")


def layout():
    if not db.rpd_path:
        return html.Div("No RPD file loaded.")

    try:
        df = db.query_df("SELECT tag, value FROM rocpd_metadata")

        if df.empty:
            return html.Div([html.H2("Metadata"), html.P("No metadata found.")])

        return html.Div([
            html.H2("Metadata"),
            dag.AgGrid(
                rowData=df.to_dict("records"),
                columnDefs=[
                    {"field": "tag", "headerName": "Tag", "flex": 1},
                    {"field": "value", "headerName": "Value", "flex": 3},
                ],
                defaultColDef={"sortable": True, "resizable": True, "filter": True},
                style={"height": "600px"},
            ),
        ])
    except Exception as e:
        return html.Div(f"Error loading metadata: {e}")
