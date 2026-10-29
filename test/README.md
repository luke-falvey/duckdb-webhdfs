# Tests

| Where | What | Needs |
|---|---|---|
| `test/cpp` | URL, glob, Hadoop config and JSON unit tests (`make test_unit`) | a C++17 compiler |
| `test/sql/*.test` | SQL tests without a cluster: URL errors, secrets, settings, httpfs loading, unreachable NameNodes | a build |
| `test/sql/integration` | reads, writes, metadata functions, several clusters, failover, DataNode maps, the HttpFS gateway | the Docker clusters (`WEBHDFS_TEST_RUNNING=1`) |
| `test/sql/kerberos` | SPNEGO reads and writes, the no-ticket error | the Kerberized cluster (`WEBHDFS_KERBEROS_TEST_RUNNING=1`) |
| `test/bench` | wall time and request counts | the Docker clusters |

## Docker clusters

`test/scripts/webhdfs_up.sh` starts two single-node Hadoop 3.4 clusters (`test/docker`) and seeds them:

- `webhdfs://localhost:9870` (DataNode HTTP on 9864) with an HttpFS gateway on `localhost:14000`;
- `webhdfs://localhost:19870` (DataNode HTTP on 19864) for the multi-cluster tests;
- a fake standby NameNode on `localhost:19999` (`test/scripts/standby_namenode.py`) for the failover tests.

The DataNodes register as `localhost`, so the redirects they hand out work from the host. `test/hadoop-conf` holds
the client `hdfs-site.xml` with the HA nameservices the tests use (`testcluster`, `deadcluster`).

```sh
test/scripts/webhdfs_up.sh
WEBHDFS_TEST_RUNNING=1 HADOOP_CONF_DIR=$PWD/test/hadoop-conf build/release/test/unittest "test/sql/integration/*"
test/scripts/webhdfs_down.sh
```

## Kerberos

`test/scripts/kerberos_up.sh` starts an MIT KDC as the current user (port 18888, files under `build/kerberos`), a
Kerberized single-node cluster on `webhdfs://localhost:29870` (`test/docker-kerberos`, host network), and gets a
ticket for `alice`. It needs `krb5-kdc`, `krb5-admin-server` and `krb5-user` (Debian/Ubuntu package names).
`test/scripts/run_kerberos_tests.sh` does all of it and runs the tests.
