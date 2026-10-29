#!/usr/bin/env bash
# Start an unprivileged MIT KDC (port 18888) and a Kerberos-secured HDFS (NameNode HTTP on 29870), seed /kerb, and
# get a ticket for alice. Needs Docker and MIT Kerberos (Debian/Ubuntu: krb5-kdc krb5-admin-server krb5-user).
# Use the printed KRB5_CONFIG / KRB5CCNAME for the client.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
K="$ROOT/build/kerberos"
REALM=EXAMPLE.COM

if [ -f "$K/krb5kdc.pid" ]; then kill "$(cat "$K/krb5kdc.pid")" 2>/dev/null || true; fi
docker compose -f "$ROOT/test/docker-kerberos/docker-compose.yml" down > /dev/null 2>&1 || true
rm -rf "$K" && mkdir -p "$K"

cat > "$K/krb5.conf" <<CONF
[libdefaults]
  default_realm = $REALM
  dns_lookup_kdc = false
  dns_lookup_realm = false
  rdns = false
  dns_canonicalize_hostname = false
  qualify_shortname = ""
  udp_preference_limit = 1
  ticket_lifetime = 24h
  forwardable = true
  default_tkt_enctypes = aes256-cts-hmac-sha1-96 aes128-cts-hmac-sha1-96
  default_tgs_enctypes = aes256-cts-hmac-sha1-96 aes128-cts-hmac-sha1-96
  permitted_enctypes = aes256-cts-hmac-sha1-96 aes128-cts-hmac-sha1-96
[realms]
  $REALM = {
    kdc = localhost:18888
  }
CONF
cat > "$K/kdc.conf" <<CONF
[kdcdefaults]
  kdc_ports = 18888
  kdc_tcp_ports = 18888
[realms]
  $REALM = {
    database_name = $K/principal
    key_stash_file = $K/stash
    acl_file = $K/kadm5.acl
    max_life = 24h
    max_renewable_life = 7d
    supported_enctypes = aes256-cts-hmac-sha1-96:normal aes128-cts-hmac-sha1-96:normal
  }
[logging]
  kdc = FILE:$K/kdc.log
CONF
touch "$K/kadm5.acl"
export KRB5_CONFIG="$K/krb5.conf" KRB5_KDC_PROFILE="$K/kdc.conf"
kdb5_util create -s -r $REALM -P masterpassword > /dev/null
for p in nn/localhost dn/localhost HTTP/localhost; do
  kadmin.local -q "addprinc -randkey $p@$REALM" > /dev/null
done
kadmin.local -q "addprinc -pw alicepassword alice@$REALM" > /dev/null
kadmin.local -q "addprinc -pw bobpassword bob@$REALM" > /dev/null
kadmin.local -q "ktadd -k $K/hdfs.keytab nn/localhost@$REALM dn/localhost@$REALM HTTP/localhost@$REALM" > /dev/null
chmod 644 "$K/hdfs.keytab"
krb5kdc -n -P "$K/krb5kdc.pid" > "$K/krb5kdc.out" 2>&1 &
echo $! > "$K/krb5kdc.pid"
sleep 1

docker compose -f "$ROOT/test/docker-kerberos/docker-compose.yml" up -d
for _ in $(seq 1 90); do
  if [[ "$(docker exec webhdfs-test-kerberos bash -c 'KRB5_CONFIG=/kerberos/krb5.conf kinit -kt /kerberos/hdfs.keytab nn/localhost && HADOOP_OPTS=-Djava.security.krb5.conf=/kerberos/krb5.conf KRB5_CONFIG=/kerberos/krb5.conf hdfs dfsadmin -report' 2>/dev/null)" == *"Live datanodes (1)"* ]]; then
    break
  fi
  sleep 2
done

# seed as the HDFS superuser (nn, the NameNode's principal)
docker exec webhdfs-test-kerberos bash -e -c '
export KRB5_CONFIG=/kerberos/krb5.conf HADOOP_OPTS=-Djava.security.krb5.conf=/kerberos/krb5.conf
kinit -kt /kerberos/hdfs.keytab nn/localhost
hdfs dfsadmin -safemode wait > /dev/null
hdfs dfs -mkdir -p /kerb/private
printf "id,name\n1,kerberos\n2,spnego\n" > /tmp/k.csv
hdfs dfs -put -f /tmp/k.csv /kerb/data.csv
hdfs dfs -put -f /tmp/k.csv /kerb/private/secret.csv
hdfs dfs -chmod 777 /kerb
hdfs dfs -chmod 700 /kerb/private
'

# a ticket for alice, in a cache of its own
KRB5CCNAME="FILE:$K/alice.ccache" kinit alice@$REALM <<< alicepassword > /dev/null
echo "Kerberized HDFS is up on webhdfs://localhost:29870. For the client:"
echo "  export KRB5_CONFIG=$K/krb5.conf KRB5CCNAME=FILE:$K/alice.ccache"
