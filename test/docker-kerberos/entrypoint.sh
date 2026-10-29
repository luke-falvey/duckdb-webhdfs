#!/bin/bash
# Starts a Kerberos-secured NameNode and DataNode. Keytabs and krb5.conf come from /kerberos (the host's KDC).
set -e
cp /conf/core-site.xml /conf/hdfs-site.xml "$HADOOP_CONF_DIR/"
export KRB5_CONFIG=/kerberos/krb5.conf
export HADOOP_OPTS="-Djava.security.krb5.conf=/kerberos/krb5.conf"
mkdir -p /tmp/hadoop-kerberos
hdfs namenode -format -force -nonInteractive > /tmp/hadoop-kerberos/format.log 2>&1
hdfs --daemon start namenode
hdfs --daemon start datanode
echo "Kerberized HDFS started"
exec tail -F /var/log/hadoop/*.out
