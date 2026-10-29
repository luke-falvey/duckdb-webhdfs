#!/bin/bash
# Starts a single-node HDFS (NameNode + DataNode, optionally HttpFS) with the config files in /conf.
set -e
cp /conf/core-site.xml /conf/hdfs-site.xml "$HADOOP_CONF_DIR/"
if [ -f /conf/httpfs-site.xml ]; then
  cp /conf/httpfs-site.xml "$HADOOP_CONF_DIR/"
fi
mkdir -p /tmp/hadoop
hdfs namenode -format -force -nonInteractive > /tmp/hadoop/format.log 2>&1
hdfs --daemon start namenode
hdfs --daemon start datanode
if [ "$WEBHDFS_START_HTTPFS" = "1" ]; then
  hdfs --daemon start httpfs
fi
echo "HDFS started"
exec tail -F /var/log/hadoop/*.log
