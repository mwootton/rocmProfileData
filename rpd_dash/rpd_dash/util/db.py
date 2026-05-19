import sqlite3
import pandas as pd

rpd_path = None


def get_connection():
    if rpd_path is None:
        raise RuntimeError("No RPD file loaded")
    return sqlite3.connect(rpd_path)


def query_df(sql, params=None):
    conn = get_connection()
    try:
        return pd.read_sql_query(sql, conn, params=params)
    finally:
        conn.close()


def table_exists(name):
    if rpd_path is None:
        return False
    conn = get_connection()
    try:
        cur = conn.execute(
            "SELECT count(*) FROM sqlite_master WHERE type IN ('table','view') AND name=?",
            (name,),
        )
        return cur.fetchone()[0] > 0
    finally:
        conn.close()
