-- MariaDB dump 10.19  Distrib 10.5.23-MariaDB, for Linux (x86_64)
--
-- Host: localhost    Database: mails
-- ------------------------------------------------------
-- Server version	10.5.23-MariaDB

/*!40101 SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT */;
/*!40101 SET @OLD_CHARACTER_SET_RESULTS=@@CHARACTER_SET_RESULTS */;
/*!40101 SET @OLD_COLLATION_CONNECTION=@@COLLATION_CONNECTION */;
/*!40101 SET NAMES utf8mb4 */;
/*!40103 SET @OLD_TIME_ZONE=@@TIME_ZONE */;
/*!40103 SET TIME_ZONE='+00:00' */;
/*!40014 SET @OLD_UNIQUE_CHECKS=@@UNIQUE_CHECKS, UNIQUE_CHECKS=0 */;
/*!40014 SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0 */;
/*!40101 SET @OLD_SQL_MODE=@@SQL_MODE, SQL_MODE='NO_AUTO_VALUE_ON_ZERO' */;
/*!40111 SET @OLD_SQL_NOTES=@@SQL_NOTES, SQL_NOTES=0 */;

--
-- Table structure for table `mail_indexed`
--

DROP TABLE IF EXISTS `mail_indexed`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8 */;
CREATE TABLE `mail_indexed` (
  `user_id` bigint(20) NOT NULL,
  `message_id` varchar(64) NOT NULL,
  `disk_path` text DEFAULT NULL,
  `mail_from` text DEFAULT NULL,
  `parsed_from` text DEFAULT NULL,
  `rcpt_to` text DEFAULT NULL,
  `folder` varchar(32) DEFAULT NULL,
  `subject` text DEFAULT NULL,
  `thread_id` varchar(64) DEFAULT NULL,
  `indexable_text` text DEFAULT NULL,
  `dkim_domain` text DEFAULT NULL,
  `sender_address` text DEFAULT NULL,
  `created_at` datetime DEFAULT NULL,
  `sent_at` datetime DEFAULT NULL,
  `delivered_at` datetime DEFAULT NULL,
  `seen_at` datetime DEFAULT NULL,
  `size` bigint(20) DEFAULT NULL,
  `direction` int(11) DEFAULT NULL,
  `status` int(11) DEFAULT NULL,
  `security` int(11) DEFAULT NULL,
  `attachments` int(11) DEFAULT NULL,
  `formats` int(11) DEFAULT NULL,
  `in_reply_to` varchar(64) DEFAULT NULL,
  `return_path` varchar(64) DEFAULT NULL,
  `ext_message_id` text DEFAULT NULL,
  `sender_name` text DEFAULT NULL,
  `reply_to` varchar(64) DEFAULT NULL,
  `attachment_ids` text DEFAULT NULL,
  PRIMARY KEY (`user_id`,`message_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Table structure for table `mail_outgoing_queue`
--

DROP TABLE IF EXISTS `mail_outgoing_queue`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8 */;
CREATE TABLE `mail_outgoing_queue` (
  `user_id` bigint(20) NOT NULL,
  `message_id` varchar(64) NOT NULL,
  `target_email` varchar(64) NOT NULL,
  `target_server` varchar(48) DEFAULT NULL,
  `disk_path` text DEFAULT NULL,
  `status` int(11) DEFAULT NULL,
  `last_retried_at` datetime DEFAULT NULL,
  `retries` int(11) DEFAULT NULL,
  `session_id` bigint(20) DEFAULT NULL,
  `locked` bigint(20) DEFAULT NULL,
  PRIMARY KEY (`user_id`,`message_id`,`target_email`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Table structure for table `mail_users`
--

DROP TABLE IF EXISTS `mail_users`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8 */;
CREATE TABLE `mail_users` (
  `user_id` bigint(20) NOT NULL AUTO_INCREMENT,
  `email` varchar(64) DEFAULT NULL,
  `password` text DEFAULT NULL,
  `created_at` datetime DEFAULT NULL,
  `used_space` bigint(20) DEFAULT NULL,
  `quota` bigint(20) DEFAULT NULL,
  PRIMARY KEY (`user_id`)
) ENGINE=InnoDB AUTO_INCREMENT=1 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;
/*!40101 SET character_set_client = @saved_cs_client */;

/*!40103 SET TIME_ZONE=@OLD_TIME_ZONE */;

/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;
/*!40014 SET UNIQUE_CHECKS=@OLD_UNIQUE_CHECKS */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
/*!40111 SET SQL_NOTES=@OLD_SQL_NOTES */;

-- Dump completed on 2024-01-01 19:44:01
