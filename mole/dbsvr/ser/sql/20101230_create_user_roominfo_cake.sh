#!/bin/bash
user="root"
password="ta0mee"
host="localhost"
tmp_file="table.sql"
create_roominfo_user_cake_table_sql() {
cat <<EOF >$tmp_file
CREATE TABLE IF NOT EXISTS t_roominfo_user_cake_$1(
	userid INT UNSIGNED NOT NULL DEFAULT 0,
	date INT UNSIGNED NOT NULL DEFAULT 0,
	cakeid INT UNSIGNED NOT NULL DEFAULT 0,
	state INT UNSIGNED NOT NULL DEFAULT 0,
	level   INT UNSIGNED NOT NULL DEFAULT 0,
	primary key (userid, date,cakeid)
	)ENGINE=innodb,CHARSET=utf8;
EOF
}

db_index=0
end_index=10

while [ $db_index -lt $end_index ] ; do
	dbx=`printf "%d" $db_index`
	echo $dbx
	table_index=0
	while [ $table_index -lt 10 ] ; do
		tbx=`printf "%d" $table_index`
		create_roominfo_user_cake_table_sql $tbx 
		cat $tmp_file | mysql -u $user --password="$password" -h $host "ROOMINFO_$dbx" 
		let "table_index+=1"
	done
	let "db_index+=1"
done

