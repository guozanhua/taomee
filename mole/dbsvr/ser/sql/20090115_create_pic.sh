#!/bin/bash
#
#庄园用户的个人数据
#
#

db_index=0
user="root"
password="ta0mee"
host="localhost"
tmp_file="table.sql"

create_user_table_sql() {
cat <<EOF >$tmp_file
	CREATE TABLE IF NOT EXISTS t_user_picture_$1 (
		photoid INT UNSIGNED NOT NULL AUTO_INCREMENT,
		userid  INT UNSIGNED NOT NULL,
		albumid INT UNSIGNED NOT NULL,
		fileid  char(50),
		PRIMARY KEY (photoid)
	) ENGINE=innodb CHARSET=UTF8;
	CREATE INDEX idx_userid on t_user_picture_$1(userid,albumid);

EOF
}

db_index=0
end_index=100
if [ "$1" == ""  ] ; then
        echo need 0,1..8
        exit;
fi

if [ "$1" == "0" ]; then
    let "db_index=0"
    let "end_index=100"
else
    let "db_index=($1-1)*12+($1/2)"
    let "end_index=$1*12+(($1+1)/2)"
fi
 echo $db_index $end_index

while [ $db_index -lt $end_index ] ; do
	echo $db_index
	dbx=`printf "%02d" $db_index`
		mysqladmin -f -u $user --password="$password" -h $host CREATE "PICTURE_$dbx"
table_index=0
		while [ $table_index -lt 10 ] ; do
			tbx=`printf "%01d" $table_index`
			create_user_table_sql $tbx
			cat $tmp_file | mysql -u $user --password="$password" -h $host "PICTURE_$dbx" 
			table_index=`expr $table_index + 1`
		done
		db_index=`expr $db_index + 1`
done

