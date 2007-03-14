#!/bin/sh

if [ -z $1 ] || [ -z $2 ] || [ -z $3 ] ; then
    echo "Usage $0 <db_dir> <sqlite2_binary> <sqlite3_binary>"
    echo "You would be better off with make upgrade"
    exit
fi

echo do not run this script more than once or you will fry your data.
echo only the word yes will proceed.
echo -n proceed [yes/no]:
read in

if [ ! -z $in ] && [ $in = "yes" ] ; then
    cd $1
    for i in *.db 
      do 
      echo upgrade $i
      $2 $i .dump | $3 $i.new
      /bin/mv $i.new $i
    done
fi
