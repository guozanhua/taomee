
#!/bin/bash
user="root"
password="ta0mee"
host="localhost"
tmp_file="table.sql"

alter_table_sql() {
	cat <<EOF >$tmp_file
		alter TABLE t_user_jy_$1  
		add last_muck_date int unsigned not null default 0,
		add muck_times tinyint unsigned not null default 0;
EOF
}

db_index=0
end_index=100

while [ $db_index -lt $end_index ] ; do
dbx=`printf "%02d" $db_index`
echo $dbx
table_index=0
while [ $table_index -lt 10 ] ; do
tbx=`printf "%d" $table_index`
alter_table_sql $tbx 
		cat $tmp_file | mysql -u $user --password="$password" -h $host "USER_$dbx" 
		let "table_index+=1"
	done
	let "db_index+=1"
done

