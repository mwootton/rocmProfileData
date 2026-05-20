import sqlite3
import pandas as pd

rpd_path = None
_persistent_conn = None
_indexes_ready = False


def get_connection():
    if rpd_path is None:
        raise RuntimeError("No RPD file loaded")
    return sqlite3.connect(rpd_path)


def _get_persistent():
    global _persistent_conn, _indexes_ready
    if _persistent_conn is None or rpd_path is None:
        _indexes_ready = False
        if rpd_path is None:
            raise RuntimeError("No RPD file loaded")
        _persistent_conn = sqlite3.connect(rpd_path, check_same_thread=False)
    return _persistent_conn


def ensure_indexes():
    global _indexes_ready
    if _indexes_ready:
        return
    conn = _get_persistent()
    conn.execute("""
        CREATE TEMPORARY TABLE IF NOT EXISTS tmp_api AS
        SELECT id, pid, tid, start, end, apiName_id, domain_id, category_id
        FROM rocpd_api
    """)
    conn.execute("CREATE INDEX IF NOT EXISTS tmp_api_tid_pid_start_idx ON tmp_api(tid,pid,start)")
    conn.execute("""
        CREATE TEMPORARY TABLE IF NOT EXISTS tmp_api_ops AS
        SELECT api_id, op_id FROM rocpd_api_ops
    """)
    conn.execute("CREATE INDEX IF NOT EXISTS tmp_api_ops_idx ON tmp_api_ops(api_id, op_id)")
    _indexes_ready = True


def get_indexed_connection():
    ensure_indexes()
    return _get_persistent()


def query_df(sql, params=None):
    conn = get_connection()
    try:
        return pd.read_sql_query(sql, conn, params=params)
    finally:
        conn.close()


def query_df_indexed(sql, params=None):
    conn = get_indexed_connection()
    return pd.read_sql_query(sql, conn, params=params)


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


def set_rpd_path(path):
    global rpd_path, _persistent_conn, _indexes_ready
    if _persistent_conn is not None:
        _persistent_conn.close()
        _persistent_conn = None
    _indexes_ready = False
    rpd_path = path
