"""Load a release build of webhdfs into a stock DuckDB (the `duckdb` Python package) and use it.

The test runner links webhdfs and httpfs statically, so this is what checks the file users install: that it is
stamped for the DuckDB release, loads, works with httpfs as a separate extension, and talks to a cluster.

    python3 -I test/scripts/loadable_smoke.py EXTENSION [--httpfs PATH|install] [--cluster URL]

--httpfs install installs httpfs from DuckDB's extension repository; a path loads that file instead.
--cluster runs reads and writes against the test cluster that test/scripts/webhdfs_up.sh starts.
Run it with -I from the repository root, or the `duckdb` directory there shadows the Python package.
"""

import argparse
import os
import sys
import tempfile

import duckdb


def check(condition, message):
    if not condition:
        sys.exit(f"FAILED: {message}")
    print(f"ok: {message}")


def row(con, sql):
    return tuple(con.execute(sql).fetchall()[0])


def check_persistent_secret_without_webhdfs(extension):
    """A persistent webhdfs secret must not stop a session without webhdfs from reading its persistent secrets."""
    config = {"allow_unsigned_extensions": "true", "autoload_known_extensions": "false"}
    with tempfile.TemporaryDirectory() as secrets:
        con = duckdb.connect(config=config)
        con.execute(f"SET secret_directory = '{secrets}'")
        con.execute(f"LOAD '{os.path.abspath(extension)}'")
        con.execute(
            "CREATE PERSISTENT SECRET smoke_persistent (TYPE webhdfs, SCOPE 'webhdfs://prod', "
            "NAMENODES ['nn1:9870'], DELEGATION_TOKEN 'smoke-token')"
        )
        con.close()

        con = duckdb.connect(config=config)
        con.execute(f"SET secret_directory = '{secrets}'")
        rows = con.execute("SELECT name, type, persistent, secret_string FROM duckdb_secrets()").fetchall()
        check(
            row(con, "SELECT count(*) FROM duckdb_extensions() WHERE extension_name = 'webhdfs' AND loaded") == (0,),
            "a fresh session does not have webhdfs loaded",
        )
        check(
            [r[:3] for r in rows] == [("smoke_persistent", "webhdfs", True)],
            "a session without webhdfs reads a persistent webhdfs secret",
        )
        check("smoke-token" not in rows[0][3], "the delegation token stays redacted without webhdfs")

        # loading webhdfs afterwards still resolves the reloaded secret by whole authority
        con.execute(f"LOAD '{os.path.abspath(extension)}'")
        check(
            row(con, "SELECT name FROM which_secret('webhdfs://prod/x', 'webhdfs')") == ("smoke_persistent",),
            "the reloaded secret matches its scope",
        )
        con.execute("DROP PERSISTENT SECRET smoke_persistent")
        con.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("extension")
    parser.add_argument("--httpfs", default="install")
    parser.add_argument("--cluster")
    args = parser.parse_args()

    con = duckdb.connect(config={"allow_unsigned_extensions": "true", "autoload_known_extensions": "false"})
    version = row(con, "SELECT version()")[0]
    print(f"DuckDB {version}")

    con.execute(f"LOAD '{os.path.abspath(args.extension)}'")
    loaded = row(con, "SELECT loaded FROM duckdb_extensions() WHERE extension_name = 'webhdfs'")
    check(loaded == (True,), "webhdfs loads")
    check(
        row(con, "SELECT count(*) FROM duckdb_functions() WHERE function_name LIKE 'webhdfs_%'")[0] > 10,
        "webhdfs functions are registered",
    )
    con.execute("CREATE SECRET smoke (TYPE webhdfs, SCOPE 'webhdfs://smoke', NAMENODES ['localhost:1'])")
    check(
        row(con, "SELECT name FROM which_secret('webhdfs://smoke/x', 'webhdfs')") == ("smoke",),
        "webhdfs secrets match by authority",
    )

    check_persistent_secret_without_webhdfs(args.extension)

    # without httpfs (and with autoloading off) a request must name what is missing
    try:
        con.execute("SELECT * FROM read_csv('webhdfs://localhost:1/x.csv')").fetchall()
        check(False, "a request without httpfs fails")
    except duckdb.Error as error:
        check("INSTALL httpfs" in str(error), "a request without httpfs asks for httpfs")

    if args.httpfs == "install":
        con.execute("INSTALL httpfs")
        con.execute("LOAD httpfs")
    else:
        con.execute(f"LOAD '{os.path.abspath(args.httpfs)}'")
    check(
        row(con, "SELECT loaded FROM duckdb_extensions() WHERE extension_name = 'httpfs'") == (True,),
        "httpfs loads",
    )

    if not args.cluster:
        return
    base = args.cluster.rstrip("/")
    con.execute("SET webhdfs_user_name = 'hadoop'")
    check(
        row(con, f"SELECT count(*), sum(doubled) FROM '{base}/test/data/numbers.parquet'") == (100000, 9999900000),
        "reads a Parquet file",
    )
    check(
        row(con, f"SELECT count(*) FROM read_parquet('{base}/test/hive/**/*.parquet', hive_partitioning = true)")[0]
        == 360,
        "reads a Hive-partitioned glob",
    )
    check(
        row(con, f"SELECT count(*) FROM webhdfs_ls('{base}/test/data')")[0] >= 5,
        "lists a directory",
    )
    out = f"{base}/test/out/loadable_smoke_{os.getpid()}.parquet"
    con.execute(f"COPY (SELECT range AS id FROM range(1000)) TO '{out}'")
    check(row(con, f"SELECT sum(id) FROM '{out}'") == (499500,), "writes and reads back a file")
    check(row(con, f"SELECT webhdfs_exists('{out}')") == (True,), "the written file exists")


if __name__ == "__main__":
    main()
