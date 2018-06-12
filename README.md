# pgagent-c

pgagent-c 是以 extension、bgworker 进程方式实现的 [pgAgent](https://github.com/postgres/pgagent)，依旧使用 [pgAdmin](https://www.pgadmin.org/) 进行管理，具体使用参考[官方文档](https://www.pgadmin.org/docs/pgadmin3/1.22/pgagent.html)。



## 优势

- 兼容 pgAgent 功能。
- 以 C 语音编写，不依赖 wxWidgets 等，编译比 pgAgent 更简洁。
- 以 extension、bgworker 进程方式实现，不需要像 pgAgent 一样手动启动进程。



## 缺点

- 仅支持 Linux。
- 日志功能没有添加，后续版本可修改。
- 测试不够完善，发现问题请提交 [issues](https://github.com/yonj1e/pgagent-c/issues) 。



## 使用

**安装 pgagent-c**

```shell
git clone https://github.com/yonj1e/pgagent-c.git
cd pgagent-c

# Ensure pg_config is in your path, e.g.
export PATH=$PATH:/usr/pgsql-10/bin

make clean; make; make install;
```

**设置 pgagent-c**

要在 PostgreSQL 启动时启动 pgagent bgworker 进程，您需要将 pgagent 配置到 postgresql.conf 中的shared_preload_libraries 中。

默认情况下，pgagent bgworker 进程将其元数据表在“postgres”数据库中创建。 但是，您可以通过在postgresql.conf 中设置 pgagent.dbname 配置参数来配置此选项。

```shell
# add to postgresql.conf:
shared_preload_libraries = 'pgagent'
agent.dbname = 'postgres'
```

**重启数据库**

重新启动 PostgreSQL 后，可以看到 `bgworker: pgagent_scheduler` 进程和 extension。

```shell
 #
 6267 pts/3    S      0:00 /usr/pgsql-10/bin/postgres -D ../data
 6268 ?        Ss     0:00 postgres: logger process   
 6270 ?        Ss     0:00 postgres: checkpointer process   
 6271 ?        Ss     0:00 postgres: writer process   
 6272 ?        Ss     0:00 postgres: wal writer process   
 6273 ?        Ss     0:00 postgres: autovacuum launcher process   
 6274 ?        Ss     0:00 postgres: archiver process   
 6275 ?        Ss     0:00 postgres: stats collector process   
 6276 ?        Ss     0:00 postgres: bgworker: pgagent_scheduler   
 6277 ?        Ss     0:00 postgres: bgworker: logical replication launcher  

```

```sql
# \dx
                   List of installed extensions
    Name    | Version |   Schema   |         Description          
------------+---------+------------+------------------------------
 pgagent    | 3.4     | pgagent    | A PostgreSQL job scheduler
 plpgsql    | 1.0     | pg_catalog | PL/pgSQL procedural language
(2 rows)
```

元数据表信息及使用参考 pgAgent。



## 作者

[杨 杰](https://yonj1e.github.io/young/)： [yonj1e@163.com](mailto:yonj1e@163.com) 

