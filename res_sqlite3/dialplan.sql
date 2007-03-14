BEGIN TRANSACTION;
create table dialplan(context varchar(255), exten varchar(255), pri int, app varchar(255), data varchar(255));
INSERT INTO dialplan VALUES('phone1',099,1,'PlayBack','demo-congrats');
COMMIT;
