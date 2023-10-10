### Соединение с локальной БД

Если вы развернули локальную YDB по сценарию самостоятельного развертывания [в Docker](../../self_hosted/ydb_docker.md) с предложенной конфигурацией, то соединение с ней можно проверить командой:

```bash
{{ ydb-cli }} -e grpc://localhost:2136 -d /local scheme ls
```