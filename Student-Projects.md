Below is the list of tasks that are good for student projects (course or graduate work).

## SDK
### Support for Coordination service in Go SDK
YDB has a Coordination Service which allow your client application to elect a leader via distributed lock (similar to ZooKeeper). The task is to add support for Coordination Service to Go SDK.

Mentor: Aleksei Miasnikov (https://github.com/asmyasnikov)

### Support for sqlx.StructScan() in Go SDK
Add support for sqlx.StructScan() from github.com/jmoiron/sqlx to Go SDK.

Mentor: Aleksei Miasnikov (https://github.com/asmyasnikov)

### Implement Plugin Mechanism in C++ SDK
Add capability of loading plugins implemented via so/dll into C++ SDK.

Example: YDB supports different authorisation mechanisms, it's a good idea to implement them as plugins to keep code dependancies clear.

Mentor: Daniil Cherednik (https://github.com/dcherednik)

### Extend C++ SDK Monitoring
Out of the box monitoring for you client application is awesome. We have some ideas how to extend C++ SDK Monitoring facilities.

**TODO: detailed description**

Mentor: Daniil Cherednik (https://github.com/dcherednik)

### Safe Shutdown in C++ SDK
Current C++ sdk implementation requires to call driver.Stop(true) method at the end of program. There are some internal sdk routines which can invoke gRpc calls out of user call context but gRpc does not allow this call after exit from main function.
Such approach (to call driver.Stop(true)) is not convenient for real application because often it is difficult to control place where driver is constructed.
The simplest solution is to make driver as a singletone object. Singletone usage is reasonable here because driver is able to work with multiple databases or with multiple clusters effectively sharing threads, connections and other grpc resources.
Other solutions (using atexit fuction) are still possible to discuss.
This task requires good knowledge of multitheading programming, ability to write portability code.

Mentor:Daniil Cherednik (https://github.com/dcherednik)

## YDB CLI (Command Line Interface)
### Implement Interactive CLI
Currently YDB CLI (https://ydb.tech/en/docs/getting_started/cli) doesn't support interactive mode. Interactive mode means that you can run the `ydb` program and it will provide you a way to write queries and get responses something like the `psql` program does.

Mentor: Nikolay Perfilov (https://github.com/pnv1)

## Extend `ydb import file` Capabilities
Currently YDB CLI (https://ydb.tech/en/docs/getting_started/cli) supports CSV and TSV input formats only. There're lots of other common formats we should support here. Such as JSON, Parquet, Avro, MessagePack, Debezium (over JSON or Avro), ORC, Protobuf, and so on. You could be interested in this task
* if you want to know how modern systems serialize their data
* if you want to get experience in data transfer between such systems

Mentor: Artem Zuikov (https://github.com/4ertus2)

## YDB Core (C++)
### Optimize TEvVPut/TEvVGet message serialization
If you want to dive into YDB's core, this is the task you are looking for. Writing and reading from/to YDB Distributed Storage (DS) effectively is very important. For every write to DS YDB's component dsproxy generates several messages TEvVPut (https://github.com/ydb-platform/ydb/blob/main/ydb/core/blobstorage/vdisk/common/vdisk_events.h#L504) because we write multiple replicas or erasure parts. TEvVPut message is serialised to go into the wire. The task is to optimize TEvVPut serialization. Currently we use Google protobuf for message serialization, but options are:
1. Use google protobuf for metadata serialization only, but don't put opaque data into proto message. Put it next after protobuf message;
2. Use flat buffers;
3. Use custom protocol.
We expect that you think propose some solutions, implement them and compare performance via benchmark.

Mentor: Aleksey Stankevichus (https://github.com/the-ancient-1)

### Add LWTrace to components
YDB uses lwtrace library for tracing events in the system and to debug issues.

**TODO: add detailed description**

Mentor: Aleksey Stankevichus (https://github.com/the-ancient-1)

### Add Open Telemetry Support to YDB
**TODO: add detailed description**

Mentor: Andrey Fomichev (https://github.com/fomichev3000)

### Implement etcd API over YDB
**TODO: add detailed description**

### Add support for Debezium record format in Change Data Capture
https://github.com/ydb-platform/ydb/issues/101

Mentor: Ilnaz Nizametdinov (https://github.com/CyberROFL)

## Embedding

### Integration Airflow and YDB Federated Queries

Federated queries are the ability to take a query and provide solutions based on information from many different sources. Airflow™ pipelines are defined in Python, allowing for dynamic pipeline generation. This allows for writing code that instantiates pipelines dynamically. In this project it's neccessary to implement the node of this pipeline which communicates with YDB Federated Queries

Languages: C++, Python

Mentor: Oleg Doronin (https://github.com/dorooleg)

### Integration DBT and YDB Federated Queries

Federated queries are the ability to take a query and provide solutions based on information from many different sources. [DBT](https://www.getdbt.com/product/what-is-dbt), short for data build tool, is an open source project for managing data transformations in a data warehouse. DBT are part of the [modern data stack](https://www.getdbt.com/blog/future-of-the-modern-data-stack). In this project it's neccessary to integrate the YDB Federated Queries into DBT

Languages: Python

Mentor: Oleg Doronin (https://github.com/dorooleg)

### Integration Apache Superset and YDB Federated Queries

Federated queries are the ability to take a query and provide solutions based on information from many different sources. After the data has already been proccessed it's usefull to visualize them. There is the [Apache Superset](https://superset.apache.org/) for this purpose. Apache Superset uses the [SQL Alchemy](https://www.sqlalchemy.org/) to communcate with external systems. SQL Alchemy already supports YDB and is used in Datalens. In this project, this functionality needs to be integrated into Apache Superset

Languages: Python

Mentor: Oleg Doronin (https://github.com/dorooleg)

### Implementation YDB Kafka Connection Sink

Kafka supports different sources/sinks for communicating with external systems. It's easy to find the list of these sources/sinks [here](https://www.confluent.io/product/connectors). As we can see there is no YDB as a sink. This project involves a new implementation of the YDB sink. The example of sink implementation can be found [here](https://github.com/kakao/kafka-sink-connector/blob/master/src/main/java/com/kakao/connector/kafka/KafkaSinkTask.java)

Languages: Java

### Implementation Apache Nifi YDB Sink
[Apache NiFi](https://nifi.apache.org/) supports powerful and scalable directed graphs of data routing, transformation, and system mediation logic. In this project, you need to implement ydb as a sink for the data graph in Apache NiFi

Languages: Java

Mentor: Oleg Doronin (https://github.com/dorooleg)