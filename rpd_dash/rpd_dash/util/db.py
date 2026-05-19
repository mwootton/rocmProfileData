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


def column_exists(table, column):
    if rpd_path is None:
        return False
    conn = get_connection()
    try:
        cur = conn.execute(f"PRAGMA table_info({table})")
        return any(row[1] == column for row in cur.fetchall())
    finally:
        conn.close()


def has_annotations():
    return column_exists("rocpd_api", "domain_id")


def has_torch_ops():
    if not has_annotations():
        return False
    conn = get_connection()
    try:
        cur = conn.execute(
            "SELECT count(*) FROM rocpd_api JOIN rocpd_string ON rocpd_string.id = rocpd_api.domain_id "
            "WHERE rocpd_string.string = 'torch' LIMIT 1"
        )
        return cur.fetchone()[0] > 0
    finally:
        conn.close()
